#!/opt/bin/php-cli
<?php
global $debug;
$debug = true;
global $use_lock;
$use_lock = true;
require_once __DIR__ . '/runner_bootstrap.php';
require_once 'WhisparrAPIClient.php';
require_once 'Netrc.php';
// use whisparr\WhisparrAPIClient;
use Netrc\Netrc;
use Netrc\Exception\ParseException;
use Netrc\Exception\FileNotFoundException;
global $use_lock;
$use_lock = true;


function cachedRpcGet($rpc, $id, $fields = null, bool $useCache = true)
{
    static $cache = array();
    $key = $id . ':' . ($fields === null ? '' : implode(',', $fields));
    if ($useCache && array_key_exists($key, $cache))
        return $cache[$key];
    if (is_null($fields))
        $res = $rpc->get($id);
    else
        $res = $rpc->get($id, $fields);
    $cache[$key] = $res;
    return $res;
}

function scriptPathIsPartial(string $path): bool
{
    return str_ends_with($path, '.part');
}

function scriptParseSize(string $value): float
{
    $value = trim($value);
    if ($value === '')
        return 0.0;
    $unit = strtoupper(substr($value, -1));
    $number = (float) $value;
    return match ($unit)
    {
        'K' => $number * 1024,
        'M' => $number * 1024 * 1024,
        'G' => $number * 1024 * 1024 * 1024,
        'T' => $number * 1024 * 1024 * 1024 * 1024,
        default => $number,
    };
}

function scriptMinFreeSpace($sessArgs, string $dir): float
{
    $rules = $sessArgs->min_free_space ?? [];
    if (!is_array($rules))
        return 0.0;

    $best = 0.0;
    $bestLen = -1;
    foreach ($rules as $rule)
    {
        if (!is_string($rule) || !str_contains($rule, '='))
            continue;
        [$path, $size] = explode('=', $rule, 2);
        $path = rtrim($path, '/\\');
        if ($path === '' || !str_starts_with($dir, $path))
            continue;
        if (strlen($path) > $bestLen)
        {
            $bestLen = strlen($path);
            $best = scriptParseSize($size);
        }
    }
    return $best;
}

function scriptPathDevice(string $path): string
{
    $path = rtrim($path, '/\\');
    while ($path !== '' && !file_exists($path))
    {
        $parent = dirname($path);
        if ($parent === $path || $parent === '.')
            break;
        $path = $parent;
    }
    if ($path === '' || !file_exists($path))
        return '';
    $st = @stat($path);
    return ($st !== false && isset($st['dev'])) ? (string) $st['dev'] : '';
}

function scriptVolumeKey(string $path): string
{
    $path = NormalizeFolder($path);
    if (PHP_OS_FAMILY === 'Windows')
    {
        if (preg_match('@^([A-Za-z]:)@', $path, $m))
            return strtoupper($m[1]);
        if (preg_match('@^(\\\\\\\\[^\\\\/]+\\\\[^\\\\/]+)@', $path, $m))
            return strtolower($m[1]);
    }
    $dev = scriptPathDevice($path);
    return $dev !== '' ? 'dev:' . $dev : '';
}

function scriptSameVolume(string $a, string $b): bool
{
    $a = NormalizeFolder($a);
    $b = NormalizeFolder($b);
    if ($a === $b)
        return true;
    $ka = scriptVolumeKey($a);
    $kb = scriptVolumeKey($b);
    if ($ka !== '' && $ka === $kb)
        return true;

    // Drive letters and UNC paths can point to the same share (T:\ vs
    // \\server\share). Fall back to the filesystem device id.
    $da = scriptPathDevice($a);
    $db = scriptPathDevice($b);
    return $da !== '' && $da === $db;
}

function scriptDataBase($torrent, string $downloadDir, string $incompleteDir): string
{
    $name = (string) ($torrent->name ?? '');
    if ($name === '')
        return '';
    foreach ([$downloadDir, $incompleteDir] as $dir)
    {
        if ($dir === '')
            continue;
        $path = rtrim($dir, '/\\') . DIRECTORY_SEPARATOR . $name;
        if (is_dir($path) || is_file($path))
            return $dir;
    }
    return '';
}

function scriptFilesSource(array $files, string $downloadDir, string $incompleteDir): string
{
    foreach ($files as $file)
    {
        $name = (string) ($file['name'] ?? '');
        if ($name === '')
            continue;
        $sub = str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $name);
        foreach ([$downloadDir, $incompleteDir] as $dir)
        {
            if ($dir === '')
                continue;
            $path = rtrim($dir, '/\\') . DIRECTORY_SEPARATOR . $sub;
            if (is_file($path) || is_file($path . '.part'))
                return $dir;
        }
    }
    return '';
}

function scriptSpaceAllows($rpc, $torrent, string $dir, $sessArgs, ?string $sourceDir = null): bool
{
    global $ignoreSpaceLimits;
    if (!empty($ignoreSpaceLimits))
        return true;

    $dest = NormalizeFolder($dir);
    $name = (string) ($torrent->name ?? '');
    $src = ($sourceDir !== null && $sourceDir !== '')
        ? $sourceDir
        : scriptDataBase(
            $torrent,
            (string) ($torrent->downloadDir ?? ''),
            (string) ($torrent->incomplete_dir ?? '')
        );
    $dataAlreadyAtDestination = $name !== '' && (is_dir($dest . $name) || is_file($dest . $name));
    if ($dataAlreadyAtDestination || ($src !== '' && scriptSameVolume($src, $dest)))
        return true;

    $free = $rpc->freeSpace($dir);
    if ($free === false || $free === null)
        return true;

    $need = (float) ($torrent->totalSize ?? 0);
    return ((float) $free - $need) >= scriptMinFreeSpace($sessArgs, $dir);
}

function scriptList($value): array
{
    if (is_array($value))
        return array_values($value);
    if (is_object($value))
        return array_values(get_object_vars($value));
    return [];
}

function scriptFirstTorrent($res): ?object
{
    $torrents = $res->arguments->torrents ?? [];
    if (is_object($torrents))
    {
        if (isset($torrents->name))
            return $torrents;
        $torrents = array_values(get_object_vars($torrents));
    }
    if (!is_array($torrents) || $torrents === [])
        return null;

    return is_object($torrents[0]) ? $torrents[0] : null;
}

function scriptRemoveTempFiles($rpc, $id): void
{
    $res = $rpc->get($id, ['name', 'downloadDir', 'incomplete_dir', 'files', 'fileStats']);
    if (!TransmissionRPC::check_result($res, 'torrents'))
        return;

    $torrent = scriptFirstTorrent($res);
    if ($torrent === null)
        return;

    $files = scriptList($torrent->files ?? []);
    $stats = scriptList($torrent->fileStats ?? []);
    $dirs = array_filter([$torrent->downloadDir ?? null, $torrent->incomplete_dir ?? null]);
    $name = $torrent->name;
    $isSingleFile = count($files) === 1 && ($files[0]->name ?? null) === $name;

    foreach ($dirs as $dir)
    {
        $base = rtrim($dir, '/\\') . DIRECTORY_SEPARATOR;
        foreach ($files as $i => $file)
        {
            $sub = str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $file->name);
            $path = $isSingleFile ? $base . $name : $base . $name . DIRECTORY_SEPARATOR . $sub;
            $wanted = $stats[$i]->wanted ?? false;
            if (!$wanted && is_file($path))
                @unlink($path);
            if (is_file($path . '.part'))
                @unlink($path . '.part');
        }
    }
}

function scriptStagingHasData($rpc, $id, bool $includeParts = true): bool
{
    $res = $rpc->get($id, ['name', 'downloadDir', 'incomplete_dir', 'files', 'fileStats']);
    if (!TransmissionRPC::check_result($res, 'torrents'))
        return false;

    $torrent = scriptFirstTorrent($res);
    if ($torrent === null)
        return false;

    $files = scriptList($torrent->files ?? []);
    $stats = scriptList($torrent->fileStats ?? []);
    $dirs = array_filter([$torrent->incomplete_dir ?? null]);
    $name = $torrent->name;
    $isSingleFile = count($files) === 1 && ($files[0]->name ?? null) === $name;

    foreach ($dirs as $dir)
    {
        $base = rtrim($dir, '/\\') . DIRECTORY_SEPARATOR;
        foreach ($files as $i => $file)
        {
            $sub = str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $file->name);
            $path = $isSingleFile ? $base . $name : $base . $name . DIRECTORY_SEPARATOR . $sub;
            $wanted = $stats[$i]->wanted ?? false;
            if ($wanted && is_file($path))
                return true;
            if ($includeParts && is_file($path . '.part'))
                return true;
        }
    }
    return false;
}

function scriptRemoveIfAllowed($rpc, $id, bool $withFiles = false): void
{
    global $removeTorrentAfterScript, $removeTempFilesAfterScript, $removeAllFilesAfterScript;
    if (!$removeAllFilesAfterScript && !$removeTorrentAfterScript && !$removeTempFilesAfterScript)
        return;
    if ($removeAllFilesAfterScript)
    {
        $rpc->remove($id, true);
        return;
    }
    if (scriptStagingHasData($rpc, $id, !$removeTempFilesAfterScript))
    {
        cprintf("yellow", "Keeping torrent [ %4d ]: data is still in incomplete" . PHP_EOL, $id);
        return;
    }
    if ($removeTempFilesAfterScript)
        scriptRemoveTempFiles($rpc, $id);
    if ($removeTorrentAfterScript)
        $rpc->remove($id, false);
}

function scriptMoveRoot($rpc, $torrent, string $downloadDir, string $incompleteDir, string $destinationDir, bool $movePartials): bool
{
    global $moveCompleted, $sessArgs, $scriptSpaceBlocked;
    if ($moveCompleted)
        return false;
    $name = $torrent->name;
    $downloadDir = rtrim($downloadDir, '/\\');
    $incompleteDir = rtrim($incompleteDir, '/\\');
    $destinationDir = rtrim($destinationDir, '/\\');

    if (is_dir($downloadDir . DIRECTORY_SEPARATOR . $name) || is_file($downloadDir . DIRECTORY_SEPARATOR . $name))
        $srcBase = $downloadDir;
    elseif (is_dir($incompleteDir . DIRECTORY_SEPARATOR . $name) || is_file($incompleteDir . DIRECTORY_SEPARATOR . $name))
        $srcBase = $incompleteDir;
    else
        return false;

    if ($srcBase === $destinationDir)
        return true;

    if (!scriptSpaceAllows($rpc, $torrent, $destinationDir, $sessArgs, $srcBase))
    {
        $scriptSpaceBlocked[$torrent->id] = true;
        return false;
    }
    unset($scriptSpaceBlocked[$torrent->id]);

    $src = $srcBase . DIRECTORY_SEPARATOR . $name;
    $dst = $destinationDir . DIRECTORY_SEPARATOR . $name;

    if ($movePartials && @rename($src, $dst))
    {
        $rpc->move($torrent->id, $destinationDir, false);
        return true;
    }

    if (is_file($src))
    {
        if (!$movePartials && scriptPathIsPartial($src))
            return false;
        @mkdir(dirname($dst), 0777, true);
        if (!@copy($src, $dst))
            return false;
        @unlink($src);
    }
    elseif (is_dir($src))
    {
        $movedAny = false;
        $it = new RecursiveIteratorIterator(
            new RecursiveDirectoryIterator($src, FilesystemIterator::SKIP_DOTS),
            RecursiveIteratorIterator::SELF_FIRST
        );
        foreach ($it as $item)
        {
            $rel = substr($item->getPathname(), strlen($src) + 1);
            if (!$movePartials && str_ends_with($rel, '.part'))
                continue;
            $target = $dst . DIRECTORY_SEPARATOR . $rel;
            if ($item->isDir())
            {
                @mkdir($target, 0777, true);
            }
            else
            {
                @mkdir(dirname($target), 0777, true);
                if (!@copy($item->getPathname(), $target))
                    return false;
                @unlink($item->getPathname());
                $movedAny = true;
            }
        }
        if (!$movedAny)
            return false;
    }
    else
    {
        return false;
    }

    $rpc->move($torrent->id, $destinationDir, false);
    return true;
}
$qbHosts = ['samsung-pc', 'sergeybo02', 'transmission'];
$client = getWhisparr();

$payload = runnerTryReadPayload();
$TR_TORRENT_ID = getenv('TR_TORRENT_ID');
$TR_TORRENT_NAME = getenv('TR_TORRENT_NAME');
$TR_TORRENT_DIR = getenv('TR_TORRENT_DIR');
$TR_TORRENT_HASH = getenv('TR_TORRENT_HASH');
$qnap = 'qnap';
$make_txt = (getenv("make_txt") === "1");
$make_jpg = (getenv("make_jpg") === "1");
$make_hash = (getenv("make_hash") === "1");
// exit(0);
//$site = "localhost";
$par = new Parameters();
//$par->setsite("localhost");
$par->addParameter("site", "s|server", "set server", ParameterType::String, ParameterCount::Single, mandatory: Mandatory::Mandatory, default: "localhost")
    ->addParameter("ids", "t|id", "set torrent id", ParameterType::Number, ParameterCount::Multiple, mandatory: Mandatory::Optional, default: ($TR_TORRENT_ID !== false) ? array($TR_TORRENT_ID) : array())
    ->addParameter("MoveReady", "m|move", "move ready files", ParameterType::Boolean)
    ->addParameter("Finish", "f|finish", "finish torrent", ParameterType::Boolean)
    ->addParameter("remove_part", "r|remove", "remove part files", ParameterType::Boolean)
    ->addParameter("makeThumb", "h|thumb", "make thumb", ParameterType::Boolean, default: false)
    ->addParameter("make_jpg", "j|jpg", "create thumbnail image", ParameterType::Boolean, default: $make_jpg)
    ->addParameter("make_txt", "x|txt", "create txt description", ParameterType::Boolean, default: $make_txt)
    ->addParameter("make_hash", "v|sfv", "create sfv checksum", ParameterType::Boolean, default: $make_hash)
    ->addParameter("description", "e|description", "create descriptions via DescriptionMedia", ParameterType::Boolean, default: false)
    ->addParameter("ignore", "i|ignore", "ignore move errors", ParameterType::Boolean, default: false)
    ->addParameter("nospace", "n|nospace", "ignore free space limits", ParameterType::Boolean, default: false)
    ->Process();
$par->setSeed(false);
$par->setInfo(false);

if ($payload !== null)
{
    runnerRequireContext($payload, 'torrent');
    $payloadTorrents = runnerTorrents($payload);
    if ($payloadTorrents !== [])
        $par->ids = runnerSelectedTorrentIds($payload);

    $parameters = runnerParameters($payload);
    $par->remove_part = ($parameters['remove_part'] ?? false) === true;
    $par->makeThumb = ($parameters['make_thumb'] ?? false) === true;
    $par->make_jpg = ($parameters['make_jpg'] ?? false) === true;
    $par->make_txt = ($parameters['make_txt'] ?? false) === true;
    $par->make_hash = ($parameters['make_sfv'] ?? false) === true;
    $par->description = ($parameters['description'] ?? false) === true;
    $par->ignore = ($parameters['ignore'] ?? false) === true;
    $par->nospace = ($parameters['nospace'] ?? false) === true;

    $rpc = runnerRpc($payload);
}
else
{
    $rpc = getTransmission($par->site);
}

$ignoreSpaceLimits = (bool) ($par->nospace ?? false);
$par1 = clone $par;

if (count($par->ids) == 0 && !$par->MoveReady)
{
    cputs("light_red", "No Torrent set" . PHP_EOL);
    exit(1);
}

if ($rpc === null)
{
    cputs("light_red", "No server available" . PHP_EOL);
    exit(1);
}
$isSergeybo02 = strtolower((string) gethostname()) === 'sergeybo02';
$Import = false;
$Imported = false;
$finished = 0;
$torrents_found = 0;
if ($rpc->connected)
{
    $dirs = array();
    $fulldirs = array();
    cputs("light_green", "Connected to " . $rpc->url . PHP_EOL);
    $incomplete_dir = $rpc->incomplete_dir_enabled ? $rpc->incomplete_dir : "";
    $sess = $rpc->sget();
    $sessArgs = $sess->arguments ?? null;
    $moveCompleted = $sessArgs->move_completed_torrents_enabled ?? true;
    $movePartials = $sessArgs->move_partial_files_enabled ?? false;
    $removeTorrentAfterScript = $sessArgs->script_torrent_done_remove_torrent_enabled ?? false;
    $removeTempFilesAfterScript = $sessArgs->script_torrent_done_remove_temp_files_enabled ?? false;
    $removeAllFilesAfterScript = $sessArgs->script_torrent_done_remove_all_files_enabled ?? false;
    $forceFinish = $sessArgs->force_finish_partial_files_enabled ?? false;
    $scriptSpaceBlocked = [];
    $fields1 = array('id', 'name', 'status', 'downloadDir', 'incomplete_dir', 'labels', 'hashString', 'comment', 'totalSize');
    $fields = array('id', 'name', 'files', 'fileStats', 'wanted');
    $res = $rpc->get($par->ids, $fields1);
    if (TransmissionRPC::check_result($res, 'torrents'))
    {
        $torrents = scriptList($res->arguments->torrents ?? []);
        $torrents_found = count($torrents);
        foreach ($torrents as $torrent)
        {
            $res1 = cachedRpcGet($rpc, $torrent->id, $fields);
            if (!TransmissionRPC::check_result($res1, 'torrents'))
                continue;
            $res1Torrents = scriptList($res1->arguments->torrents ?? []);
            if ($res1Torrents === [])
                continue;
            $torrent1 = $res1Torrents[0];
            if (!(isset($torrent1->files) && is_array($torrent1->files)))
                continue;

            $par = clone $par1;
            $forceThis = $forceFinish || $par->Finish || (isset($torrent->labels) && is_array($torrent->labels) && in_array('ForceFinish', $torrent->labels));
            $TVShow = false;
            $Sonarr = false;
            $Whisparr = false;
            $Movies = false;
            $DVD = false;
            $Clips = false;
            $TR_TORRENT_ID = $torrent->id;
            $TR_TORRENT_HASH = $torrent->hashString;
            $TR_TORRENT_DIR = NormalizeFolder($torrent->downloadDir);
            $TR_TORRENT_NAME = $torrent->name;
            $TR_TORRENT_COMMENT = $torrent->comment;
            $incomplete_dir = NormalizeFolder($rpc->getStagingDir($torrent));
            $destinationDir = $downloadDir = NormalizeFolder($torrent->downloadDir);
            $Import = preg_match('@.*import[\\\\/]?$@i', $destinationDir) == 1;
            $Whisparr = preg_match('@.*whisparr[\\\\/]?$@i', $destinationDir) == 1;
            $Sonarr = preg_match('@.*sonarr[\\\\/]?$@i', $destinationDir) == 1;
            $Sonarr = $Sonarr || preg_match('@.*sonarr1[\\\\/]?$@i', $destinationDir) == 1;
            $TVShow = preg_match('@.*TV-Show[\\\\/]?$@', $destinationDir) == 1;
            $Movies = preg_match('@.*Movies[\\\\/]?$@', $destinationDir) == 1;
            $Incomplete = preg_match('@.*Incomplete[\\\/]*$@', $destinationDir) == 1;
            $Clips = preg_match('@.*Clips(.\.Hide|.\.Seed)?[\\\\/]?@', $destinationDir) == 1 || $DVD || $Incomplete;
            if (preg_match('@.*import[\\\\/]?$@i', $destinationDir) == 1)
                $par->setFinish(true);
            if (isset($torrent->labels) && is_array($torrent->labels))
            {
                if (in_array('Pack', $torrent->labels) || in_array('Clips', $torrent->labels))
                {
                    $Clips = true;
                }
                if (in_array('Whisparr', $torrent->labels))
                {
                    $Whisparr = true;
                    $Clips = false;
                    $par->setFinish(true);
                }
                if (in_array('Import', $torrent->labels))
                {
                    $Import = true;
                    if (!$Sonarr)
                        $Whisparr = true;
                    $par->setFinish(true);
                }
                if (in_array('Seed', $torrent->labels))
                {
                    $par->setSeed(true);
                }
                else
                {
                    if (in_array('Finish', $torrent->labels))
                    {
                        $par->setFinish(true);
                    }
                    if (in_array('Move', $torrent->labels))
                    {
                        $par->setMoveReady(true);
                    }
                    if (in_array('DVD', $torrent->labels))
                    {
                        $DVD = true;
                    }
                    if (in_array('TV-Show', $torrent->labels))
                    {
                        $TVShow = true;
                    }
                    if (in_array('Movies', $torrent->labels))
                    {
                        $Movies = true;
                    }

                    if (in_array('Hide', $torrent->labels))
                    {
                        if ($Incomplete || $DVD)
                        {
                            $patterns = array();
                            $replacements = array();
                            $patterns[] = "@(.*)Incomplete[\\\/]*$@";
                            if ($DVD)
                                $replacements[] = "$1DVD" . DIRECTORY_SEPARATOR . ".Hide";
                            else
                                $replacements[] = "$1Clips" . DIRECTORY_SEPARATOR . ".Hide";

                            // $downloadDir = 
                            $destinationDir = NormalizeFolder(preg_replace($patterns, $replacements, $destinationDir));
                            // scriptMoveRoot($rpc, $torrent, $downloadDir, $incomplete_dir, $destinationDir, $movePartials);
                        }
                    }
                }
            }
            if ($par->MoveReady)
            {
                $par->setFinish(false);
                $par->setSeed(false);
            }
            if ($par->Finish)
            {
                $par->setSeed(false);
                $patterns = array();
                $replacements = array();
                $patterns[] = "@[/\\\\]\.Seed.*@";
                $replacements[] = "";

                $destinationDir = NormalizeFolder(preg_replace($patterns, $replacements, $destinationDir, count: $count));
            }
            if ($par->Seed)
            {
                if ($Incomplete || $DVD)
                {
                    // printf("%s\n", $destinationDir);
                    $patterns = array();
                    $replacements = array();
                    $patterns[] = "@(.*)Incomplete[\\\/]*$@";
                    if ($DVD)
                        $replacements[] = "$1DVD/.Seed";
                    else
                        $replacements[] = "$1Clips/.Seed";
                    $destinationDir = NormalizeFolder(preg_replace($patterns, $replacements, $destinationDir));
                    // printf("%s\n", $destinationDir);
                    $fulldirs[$destinationDir] = $destinationDir;
                    $par->setInfo(true);
                    $patterns = array();
                    $replacements = array();
                    $patterns[] = "/(.*?)\.(.*)/";
                    $replacements[] = "$1";
                    $destinationDir = NormalizeFolder($destinationDir) . preg_replace($patterns, $replacements, $torrent->name);
                    // printf("%s\n", $destinationDir);
                    scriptMoveRoot($rpc, $torrent, $downloadDir, $incomplete_dir, $destinationDir, $movePartials);
                    $rpc->set($TR_TORRENT_ID, array('trackerList' => ['http://plab.site/ann?uk=BAu0w74uqP']));
                    $rpc->reannounce($TR_TORRENT_ID);
                }
                else
                {
                    $patterns = array();
                    $replacements = array();
                    $patterns[] = "@(.*\.Seed)[\\\\/]?$@";
                    $replacements[] = "$1";
                    $fulldirs[$destinationDir] = $destinationDir;
                    // printf("%s\n", $destinationDir);

                    $par->setInfo(true);
                    preg_replace($patterns, $replacements, $destinationDir, count: $count);
                    if ($count == 1)
                    {
                        $patterns = array();
                        $replacements = array();
                        $patterns[] = "/(.*?)\.(.*)/";
                        $replacements[] = "$1";
                        $destinationDir = NormalizeFolder($destinationDir) . preg_replace($patterns, $replacements, $torrent->name);
                        // printf("%s\n", $destinationDir);
                        scriptMoveRoot($rpc, $torrent, $downloadDir, $incomplete_dir, $destinationDir, $movePartials);
                        $rpc->set($TR_TORRENT_ID, array('trackerList' => ['http://plab.site/ann?uk=BAu0w74uqP']));
                        $rpc->reannounce($TR_TORRENT_ID);
                    }
                }
                $rpc->start_now($TR_TORRENT_ID);
                $new_files = array();
                foreach ($torrent1->files as $i => $file)
                {
                    $fileStat = $torrent1->fileStats[$i];
                    $bytesCompleted = isset($file->bytesCompleted) ? $file->bytesCompleted : 0;
                    $wanted = isset($fileStat->wanted) ? $fileStat->wanted : false;
                    $new_files[$i] = array(
                        "name" => $file->name,
                        "id" => $i,
                        "niceName" => $Movies ? MakeNicePath($file->name, translit: false) : ($TVShow ? MakeNiceSeason(MakeNicePath($file->name, translit: false)) : ($DVD ? MakeNiceDVD($file->name, isFile: true) : ($Clips ? MakeNicePath($file->name, isFile: true) : $file->name))),
                        "length" => $file->length,
                        "bytesCompleted" => $bytesCompleted,
                        "completed" => $bytesCompleted == $file->length,
                        "wanted" => $wanted

                    );
                }
                usort($new_files, function ($a1, $a2)
                {
                    return strcasecmp($a1["name"], $a2["name"]);
                });
                echo ($finished++) ? PHP_EOL : "";
                cputs("light_green", "Torrent marked for seeding [ $TR_TORRENT_ID ] $TR_TORRENT_NAME at " . date("d.m.Y H:i:s") . PHP_EOL);
                $files = array_filter($new_files, function ($value) use ($par): bool
                {
                    return preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $value['name']) === 1 && $value['wanted'] && $value['completed'];
                });
                // print_r($files);
                $text = "Torrent marked for seeding [ $TR_TORRENT_ID ]" . PHP_EOL . "<b>$TR_TORRENT_NAME</b>" . PHP_EOL;
                if (count($files) == 0 || !$par->makeThumb)
                {
                    TelegramSendMessage($text);
                }
                else
                {
                    $j = generateThumbnails($files, [$destinationDir], false);
                    TelegramSendMessage($text, $j);
                    foreach ($j as $f)
                        unlink($f);
                }

                if ($par->description)
                {
                    $descriptions = array();
                    DescriptionMedia($descriptions, [$destinationDir]);
                    foreach ($descriptions as $description)
                    {
                        $description->flush();
                    }
                }
                continue;
            }


            if (isset($torrent->status))
                $status = $torrent->status;
            else
                $status = TransmissionRPC::TR_STATUS_STOPPED;
            if ($TVShow)
            {
                $par->setInfo(true);
                $par->setFinish(true);
                $patterns = array();
                $replacements = array();
                $patterns[] = "/(.*).[Ss]([0-9]{1,2})E([0-9]{1,2})(.*)/";
                $replacements[] = "$1.$2";
                $patterns[] = "/\./";
                $replacements[] = "_";
                $patterns[] = "/_[0-9]{4}/";
                $replacements[] = "_";
                $patterns[] = "/(.*)_([Ss]?[0-9]+)(.*)/";
                $replacements[] = "$1.$2";
                $patterns[] = "/(.*)\.[Ss]?([0-9]{1,2}$)/";
                if (count($torrent1->files) == 1)
                    // $replacements[] = "$1\\\\$1.$2";
                    $replacements[] = "$1" . DIRECTORY_SEPARATOR . "Season $2";
                else
                    $replacements[] = "$1";
                //var_dump($torrent->name);
                $patterns[] = "/_/";
                $replacements[] = " ";
                $extraName = preg_replace($patterns, $replacements, getNice($torrent->name, debug: false, translit: false));
                // Logger::log("1 " . $destinationDir);
                if ($Incomplete)
                {
                    $patterns = array();
                    $replacements = array();
                    $patterns[] = "@(.*)Incomplete[\\\/]*$@";
                    $replacements[] = "$1TV-Show";
                    $destinationDir = NormalizeFolder(preg_replace($patterns, $replacements, $destinationDir));
                }
                // Logger::log("2 " . $destinationDir);
                if (preg_match("@TV-Show[/\\\\]?$@", $destinationDir) == 0)
                {
                    $destinationDir = NormalizeFolder($destinationDir) . "TV-Show";
                }
                // Logger::log("3 " . $destinationDir);

                if ($extraName != "")
                {
                    if (!str_ends_with($destinationDir, $extraName) && ($torrent->name != $extraName))
                        $destinationDir = NormalizeFolder($destinationDir) . $extraName;
                }
                // Logger::log("4 " . $destinationDir);
            }
            else if ($Movies)
            {
                $par->setInfo(true);
                $patterns = array();
                $replacements = array();
                $patterns[] = "/\./";
                $replacements[] = " ";
                //var_dump($torrent->name);
                if (count($torrent1->files) == 1)
                    // $replacements[] = "$1\\\\$1.$2";
                    $extraName = preg_replace($patterns, $replacements, getNice(pathinfo($torrent->name, PATHINFO_FILENAME), debug: false, translit: false));
                else
                    $extraName = preg_replace($patterns, $replacements, getNice($torrent->name, debug: false, translit: false));
                if ($Incomplete)
                {
                    $patterns = array();
                    $replacements = array();
                    $patterns[] = "@(.*)Incomplete[\\\/]*$@";
                    $replacements[] = "$1Movies";
                    $destinationDir = preg_replace($patterns, $replacements, $destinationDir);
                }
                if (preg_match("@Movies[/\\\\]?$@", $destinationDir) == 0)
                {
                    $destinationDir = NormalizeFolder($destinationDir) . "Movies";
                }

                if ($extraName != "")
                {
                    if (!str_ends_with($destinationDir, $extraName) && ($torrent->name != $extraName))
                        $destinationDir = NormalizeFolder($destinationDir) . $extraName;
                }
            }
            else if ($Incomplete)
            {
                $par->setInfo(true);
                $patterns = array();
                $replacements = array();
                $patterns[] = "@(.*)(Incomplete[\\\/]*)$@";
                if ($DVD)
                    $replacements[] = "$1DVD";
                else if ($Whisparr)
                    $replacements[] = "$1$2Whisparr";
                else
                {
                    $replacements[] = "$1Clips";
                }
                $destinationDir = NormalizeFolder(preg_replace($patterns, $replacements, $destinationDir));
                // global $sript_dir;
                // $f = $sript_dir . DIRECTORY_SEPARATOR . "extra_dir.txt";
                // if (($par->Finish || $par->MoveReady) && is_file($f))
                // {
                //     $destinationDir = $destinationDir . file_get_contents($f);
                // }
                // var_dump($destinationDir);
            }
            else if ($Whisparr)
            {
                $par->setInfo(true);
                $patterns = array();
                $replacements = array();
                $patterns[] = "@(.*)(clips[\\\/]*)$@i";
                $replacements[] = "$1Incomplete" . DIRECTORY_SEPARATOR . "Whisparr";
                $destinationDir = NormalizeFolder(preg_replace($patterns, $replacements, $destinationDir));
            }
            $destinationDir = NormalizeFolder($destinationDir);
            if ($DVD)
            {
                $par->setInfo(true);
                $d1 = getNice($torrent->name);
                $patterns = array();

                $replacements = array();
                $patterns[] = "/(.*?)(\.[0-9]+)([\.0-9\-]*|$)/";
                if (count($torrent1->files) != 1)
                {
                    $replacements[] = "$1";
                    // $patterns[] = "/(.*?)(\.[0-9]+)$/";
                    // // $replacements[] = "$1\\\\$1$2";
                    // $replacements[] = "$1";
                }
                else
                {
                    $d1 = pathinfo($d1, PATHINFO_FILENAME);
                    $replacements[] = "$1\\\\$1$2$3";
                }
                $extraName = MakeNiceDVD($d1);
                $extraName = NormalizeFolder(preg_replace($patterns, $replacements, $extraName));
                if ($extraName != "")
                {
                    if (!str_ends_with($destinationDir, $extraName) && ($torrent->name != $extraName))
                        $destinationDir = NormalizeFolder($destinationDir) . $extraName;
                }

            }
            else if ($Clips)
            {
                $par->setInfo(true);
                global $sript_dir;
                if (!in_array('Hide', is_array($torrent->labels) ? $torrent->labels : []))
                {
                    $f = $sript_dir . DIRECTORY_SEPARATOR . "extra_dir.txt";
                    if (($par->Finish || $par->MoveReady) && is_file($f))
                    {
                        $destinationDir = NormalizeFolder($destinationDir) . file_get_contents($f);
                    }
                }
                $patterns = array();
                $replacements = array();
                $patterns[] = "/(.*?)\.(.*)/";
                $replacements[] = "$1";
                $extraName = preg_replace($patterns, $replacements, $torrent->name);
                $parse = $client->parseSeries($extraName );
                if ($parse !== false && isset($parse->series) && !is_null($parse->series) && isset($parse->series->title))
                {
                    $extraName = $parse->series->title;
                }
                $destinationDir = NormalizeFolder($destinationDir) . $extraName;
            }
            if ($Whisparr && preg_match('@Whisparr[\\\\/]?$@i', $destinationDir) == 1)
            {
                $par->setInfo(true);
                $extraName = "";
                $parse = parseSeries($client, $torrent->name);
                if ($parse !== false && isset($parse->series) && !is_null($parse->series) && isset($parse->series->title) && $parse->series->title != "")
                {
                    $extraName = $parse->series->title;
                }
                if ($extraName != "" && !str_ends_with(NormalizeFolder($destinationDir), $extraName) && $torrent->name != $extraName)
                {
                    $destinationDir = NormalizeFolder($destinationDir) . NormalizeFolder($extraName);
                }
            }
            $destinationDir = NormalizeFolder($destinationDir);
            if ($par->Info)
                $fulldirs[$destinationDir] = $destinationDir;
            // var_dump($fulldirs);
            //var_dump($destinationDir);
            //print_r($torrent->files);
            $new_files = array();
            foreach ($torrent1->files as $i => $file)
            {
                $fileStat = $torrent1->fileStats[$i];
                $bytesCompleted = isset($file->bytesCompleted) ? $file->bytesCompleted : 0;
                $wanted = isset($fileStat->wanted) ? $fileStat->wanted : false;
                $new_files[$i] = array(
                    "name" => $file->name,
                    "id" => $i,
                    "niceName" => $Movies ? MakeNicePath($file->name) : ($TVShow ? MakeNiceSeason(MakeNicePath($file->name)) : ($DVD ? MakeNiceDVD($file->name, isFile: true) : (($Clips) ? MakeNicePath($file->name, isFile: true) : $file->name))),
                    "length" => $file->length,
                    "bytesCompleted" => $bytesCompleted,
                    "completed" => $bytesCompleted == $file->length,
                    "wanted" => $wanted

                );
            }
            usort($new_files, function ($a1, $a2)
            {
                return strcasecmp($a1["name"], $a2["name"]);
            });
            // print_r($new_files);
            if ($par->MoveReady)
            {
                if (($torrent->status == TransmissionRPC::TR_STATUS_CHECK) || ($torrent->status == TransmissionRPC::TR_STATUS_CHECK_WAIT))
                {
                    cprintf("yellow", "Check is scheduled in torrent  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    continue;
                }
                if (!in_array(true, array_column($new_files, 'completed')))
                {
                    cprintf("yellow", "No complete files in torrent  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    continue;
                }
            }
            else if (!$par->Finish)
            {
                if (in_array(false, array_column($new_files, 'completed')))
                {
                    cprintf("yellow", "Not complete torrent  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    continue;
                }
            }
            // else if ($par->Finish && $par->remove_part)
            // {
            //     if (in_array(false, array_column(array_filter($new_files, 'cmpWanted'), 'completed')) )
            //     {
            //         cputs("yellow", "Some wanted files are not finished" . PHP_EOL);
            //         sleep(5);
            //         continue;
            //     }
            // }
            if (!is_dir($destinationDir))
                mk_dir($destinationDir, 0777, true);

            for ($m = 0, $i = 0; $m <= 0 && $i < 5; $i++, sleep(5))
            {
                $m = $rpc->freeSpace($destinationDir);
            }
            $free_space = ($m !== false) ? printSize($m) : "Unknown";
            if ($par->MoveReady)
            {
                echo ($finished++) ? PHP_EOL : "";
                cprintf("light_green", "Move ready torrent [ $TR_TORRENT_ID ] $TR_TORRENT_NAME at " . date("d.m.Y H:i:s") . PHP_EOL . "Destination => $destinationDir ($free_space)" . PHP_EOL);
                $text = "Move ready torrent [ $TR_TORRENT_ID ]" . PHP_EOL . "<b>$TR_TORRENT_NAME</b>" . PHP_EOL . "$destinationDir ($free_space)" . PHP_EOL;
                $files = array_filter($new_files, function ($value) use ($par): bool
                {
                    //                    return preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $value['name']) === 1 && $value['wanted'] && $value['completed'];
                    return $value['wanted'] && $value['completed'];
                });
                // print_r($files);
                if (count($files) == 0 || !$par->makeThumb)
                {
                    TelegramSendMessage($text);
                }
                else
                {
                    $j = generateThumbnails($files, [$incomplete_dir, $downloadDir], $par->Finish);
                    TelegramSendMessage($text, $j);
                    foreach ($j as $f)
                        unlink($f);
                }
            }
            else if (!$par->Finish)
            {
                echo ($finished++) ? PHP_EOL : "";
                cputs("light_green", "Finish torrent [ $TR_TORRENT_ID ] $TR_TORRENT_NAME at " . date("d.m.Y H:i:s") . PHP_EOL . "Destination => $destinationDir ($free_space)" . PHP_EOL);
                $text = "Finish torrent [ $TR_TORRENT_ID ]" . PHP_EOL . "<b>$TR_TORRENT_NAME</b>" . PHP_EOL . "$destinationDir ($free_space)" . PHP_EOL;
                $files = array_filter($new_files, function ($value) use ($par): bool
                {
                    return preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $value['name']) === 1 && $value['wanted'] && $value['completed'];
                });
                if (count($files) == 0 || !$par->makeThumb)
                {
                    TelegramSendMessage($text);
                }
                else
                {
                    $j = generateThumbnails($files, [$incomplete_dir, $downloadDir], $par->Finish);
                    TelegramSendMessage($text, $j);
                    foreach ($j as $f)
                        unlink($f);
                }
            }
            else if ($par->Finish)
            {
                echo ($finished++) ? PHP_EOL : "";
                cputs("light_green", "Finish torrent [ $TR_TORRENT_ID ] $TR_TORRENT_NAME at " . date("d.m.Y H:i:s") . PHP_EOL . "Destination => $destinationDir ($free_space)" . PHP_EOL);
                $text = "Finish torrent [ $TR_TORRENT_ID ]" . PHP_EOL . "<b>$TR_TORRENT_NAME</b>" . PHP_EOL . "$destinationDir ($free_space)" . PHP_EOL;
                $files = array_filter($new_files, function ($value) use ($par): bool
                {
                    return preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $value['name']) === 1 && $value['wanted'];
                });
                if (count($files) == 0 || !$par->makeThumb)
                {
                    TelegramSendMessage($text);
                }
                else
                {
                    $j = generateThumbnails($files, [$incomplete_dir, $downloadDir], $par->Finish);
                    TelegramSendMessage($text, $j);
                    foreach ($j as $f)
                        unlink($f);
                }
            }
            // exit(0);
            if (in_array($status, array(TransmissionRPC::TR_STATUS_CHECK, TransmissionRPC::TR_STATUS_CHECK_WAIT)))
            {
                cprintf("yellow", "Check is scheduled in torrent  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                continue;
            }
            // If the data is still in staging, move it here when the daemon's
            // completion move is disabled. Keep the torrent if the move is
            // blocked by min-free-space or fails.
            $stagingHasData = is_dir($incomplete_dir . $torrent->name)
                || (isset($torrent1->files[0]) && (is_file($incomplete_dir . $torrent1->files[0]->name)
                    || is_file($incomplete_dir . $torrent1->files[0]->name . '.part')));
            if ($stagingHasData && NormalizeFolder($downloadDir) !== NormalizeFolder($incomplete_dir)
                && !$par->Finish && !$forceThis)
            {
                if ($moveCompleted)
                {
                    cprintf("yellow", "Waiting for Transmission to move [ %4d ] %s from incomplete" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    continue;
                }

                if (!$par->MoveReady)
                {
                    cprintf("light_green", "Moving [ %4d ] %s from incomplete to %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME, $destinationDir);
                    if (!scriptMoveRoot($rpc, $torrent, $downloadDir, $incomplete_dir, $destinationDir, $movePartials))
                    {
                        cprintf("yellow", "Not enough free space or move failed [ %4d ] %s: keeping torrent and data" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                        continue;
                    }

                    cprintf("light_green", "Moved [ %4d ] %s to %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME, $destinationDir);
                    // Do not remove torrent in move-ready mode
                    continue;
                }
            }
            if ($status != TransmissionRPC::TR_STATUS_STOPPED)
                $rpc->stop($TR_TORRENT_ID);
            // if (!($Clips || $TVShow || $Movies || $par->MoveReady || $par->Finish || $Sonarr))
            if (!($Clips || $TVShow || $Movies || $par->MoveReady || $par->Finish || $Whisparr || $Sonarr))
            {
                exit(0);
            }
            if ($Whisparr && !$par->MoveReady)
            {
                $files = array_filter($new_files, function ($value): bool
                {
                    return $value['wanted'];
                });

                if (!(isset($torrent1->files) && is_array($torrent1->files)))
                    continue;

                if (count($torrent1->files) > 1)
                {
                    if (is_dir($incomplete_dir . $torrent->name))
                    {
                        scriptMoveRoot($rpc, $torrent, $downloadDir, $incomplete_dir, $destinationDir, $movePartials);
                    }
                }
                else
                {
                    if (is_file($incomplete_dir . $torrent1->files[0]->name) || is_file($incomplete_dir . $torrent1->files[0]->name . ".part"))
                    {
                        scriptMoveRoot($rpc, $torrent, $downloadDir, $incomplete_dir, $destinationDir, $movePartials);
                    }
                }
                $files1 = array_filter($torrent1->files, function ($file)
                {
                    return preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $file->name) === 1;
                });
                if (!$Whisparr && (count($files1) == 1) && count($torrent1->files) > 1)
                {
                    $file = array_first($files1)->name;

                    $rpc->rename($torrent->id, $file, $torrent->name . "." . pathinfo($file, PATHINFO_EXTENSION));
                    $res1 = cachedRpcGet($rpc, $torrent->id, $fields, false);
                    // print_r($res1);
                    if (!TransmissionRPC::check_result($res1, 'torrents'))
                        continue;
                    $res1Torrents = scriptList($res1->arguments->torrents ?? []);
                    if ($res1Torrents === [])
                        continue;
                    $torrent1 = $res1Torrents[0];
                    if (!(isset($torrent1->files) && is_array($torrent1->files)))
                        continue;
                    $new_files = array();
                    foreach ($torrent1->files as $i => $file)
                    {
                        $fileStat = $torrent1->fileStats[$i];
                        $bytesCompleted = isset($file->bytesCompleted) ? $file->bytesCompleted : 0;
                        $wanted = isset($fileStat->wanted) ? $fileStat->wanted : false;
                        $new_files[$i] = array(
                            "name" => $file->name,
                            "id" => $i,
                            "niceName" => $Movies ? MakeNicePath($file->name) : ($TVShow ? MakeNiceSeason(MakeNicePath($file->name)) : ($DVD ? MakeNiceDVD($file->name, isFile: true) : (($Clips) ? MakeNicePath($file->name, isFile: true) : $file->name))),
                            "length" => $file->length,
                            "bytesCompleted" => $bytesCompleted,
                            "completed" => $bytesCompleted == $file->length,
                            "wanted" => $wanted

                        );
                    }
                    usort($new_files, function ($a1, $a2)
                    {
                        return strcasecmp($a1["name"], $a2["name"]);
                    });
                }
                $spaceSource = scriptFilesSource($new_files, NormalizeFolder($downloadDir), $incomplete_dir);
                if (!scriptSpaceAllows($rpc, $torrent, $destinationDir, $sessArgs, $spaceSource))
                {
                    cputs("yellow", "Not enough free space for " . $torrent->name . PHP_EOL);
                    continue;
                }
                foreach ($new_files as $file)
                {
                    foreach ([NormalizeFolder($torrent->downloadDir), $incomplete_dir] as $dir)
                    {
                        foreach ([$file['name'], $file['name'] . ".part"] as $fname)
                        {
                            if (str_ends_with($fname, '.part') && !$movePartials && !$forceThis)
                                continue;
                            if (isFile($dir . $fname))
                            {
                                if ($file['wanted'])
                                {
                                    moveFile($dir . $fname, $destinationDir . $file['name']);
                                }
                                elseif ($removeTempFilesAfterScript || $removeAllFilesAfterScript)
                                {
                                    unlink($dir . $fname);
                                }
                            }
                        }
                    }
                }
                foreach ([NormalizeFolder($torrent->downloadDir), $incomplete_dir] as $dir)
                {
                    if ($dir != $destinationDir && is_dir($dir . $torrent->name))
                        if ($removeAllFilesAfterScript)
                            delTree($dir . $torrent->name);
                }
                if ($Import)
                {
                    cprintf("light_green", "Import torrent to Whisparr  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    $scanDirs = [];
                    foreach ($new_files as $file)
                    {
                        if (!$file['wanted'])
                            continue;
                        $candidate = $destinationDir . str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $file['name']);
                        if (is_file($candidate))
                        {
                            $scanDirs[dirname($candidate)] = dirname($candidate);
                        }
                    }
                    uasort($scanDirs, function ($a, $b): int
                    {
                        $depth = substr_count(NormalizeFolder($b), DIRECTORY_SEPARATOR) <=> substr_count(NormalizeFolder($a), DIRECTORY_SEPARATOR);
                        if ($depth !== 0)
                        {
                            return $depth;
                        }
                        return strnatcasecmp($a, $b);
                    });
                    $importedCount = 0;
                    foreach ($scanDirs as $scanDir)
                    {
                        $importedCount += import2Whisparr($client, $scanDir, downloadId: $TR_TORRENT_HASH);
                    }
                    $Imported = true;
                    if ($importedCount > 0 && is_dir($destinationDir . $TR_TORRENT_NAME))
                    {
                        if ($removeAllFilesAfterScript)
                            CleanUpDirectory($destinationDir . $TR_TORRENT_NAME);
                    }
                    // renameEpisodes($client, $ids, pathinfo($argv[0], PATHINFO_DIRNAME));
                    // checkDownloadedEpisodes($client);
                    $r = cachedRpcGet($rpc, $TR_TORRENT_ID, null);
                    if (TransmissionRPC::check_result($r, 'torrents'))
                    {
                        foreach (scriptList($r->arguments->torrents ?? []) as $torrent)
                        {
                            cprintf("light_green", PHP_EOL . "Remove torrent  [ %4d ] %s" . PHP_EOL, $torrent->id, $torrent->name);
                            scriptRemoveIfAllowed($rpc, $torrent->id, false);
                        }
                    }
                }
                if ($torrent->downloadDir != $destinationDir)
                    scriptRemoveIfAllowed($rpc, $TR_TORRENT_ID, true);
                else
                    scriptRemoveIfAllowed($rpc, $TR_TORRENT_ID, false);
                foreach ($qbHosts as $qbHost)
                    removeQbittorrentDuplicate($TR_TORRENT_HASH, host: $qbHost);
                continue;
            }
            $removeDir = false;
            if ($Clips || $TVShow || $Movies || $par->MoveReady)
            {
                if (count($new_files) > 0)
                {
                    $media = preg_grep(AdvancedRecursiveDirectoryIterator::mediaMask, array_column($new_files, 'name'));
                    // if ($media != false)
                    // {
                    $media = count($media) == 1;
                    //for ($i = 0; $i < count($new_files); $i++)
                    foreach ($new_files as &$file)
                    {
                        if ($file['wanted'])
                        {
                            if ((!$par->MoveReady) && (@preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $file['name']) == false))
                            {
                                $file['wanted'] = false;
                                continue;
                            }
                            if ($media && $Clips)
                            {
                                $niceName = $file['niceName'];
                                $path = pathinfo($niceName);
                                if ($path['dirname'] == '.')
                                    continue;
                                $ext = "." . pathinfo($file['name'], PATHINFO_EXTENSION);
                                // if (!array_key_exists('extension', $path))
                                //     $ext = "";
                                // else
                                //     $ext = "." . $path['extension'];
                                $d = preg_replace(AdvancedRecursiveDirectoryIterator::mediaMask, "$1", preg_replace("@[/\\\\].*@", "", $path['dirname']));
                                if ($d == ".")
                                    continue;
                                echo "Old niceName=$niceName" . PHP_EOL;
                                if ($DVD)
                                    $niceName = $d . DIRECTORY_SEPARATOR . $d . $ext;
                                else
                                    $niceName = $d . $ext;
                                $file['niceName'] = $niceName;
                                echo "New niceName=$niceName" . PHP_EOL;
                            }
                            if ($media && ($Whisparr || $Import))
                            {
                                $niceName = $file['niceName'];
                                $path = pathinfo($niceName);
                                if ($path['dirname'] == '.')
                                    continue;
                                $file['niceName'] = $path['basename'];
                                $removeDir = true;
                            }
                        }
                    }
                    unset($file);
                    // }
                }
            }
            $Completed = array();
            $Removed = array();
            $Failed = array();
            $l = 0;
            array_walk($new_files, function ($file) use ($incomplete_dir, $downloadDir, $par, &$l): void
            {
                if ($file['wanted'])
                {
                    if ($par->MoveReady && !$file['completed'])
                    {
                        return;
                    }
                    foreach ([NormalizeFolder($downloadDir), $incomplete_dir] as $dir)
                    {
                        foreach ([$file['name'], $file['name'] . ".part"] as $fname)
                        {
                            if (isFile($dir . $fname))
                            {
                                if (strlen($dir . $fname) > $l)
                                    $l = strlen($dir . $fname);
                            }
                        }
                    }
                }
            });
            $spaceSource = scriptFilesSource($new_files, NormalizeFolder($downloadDir), $incomplete_dir);
            if (!scriptSpaceAllows($rpc, $torrent, $destinationDir, $sessArgs, $spaceSource))
            {
                cputs("yellow", "Not enough free space for " . $torrent->name . PHP_EOL);
                continue;
            }
            foreach ($new_files as $file)
            {
                if ($forceThis && $file['wanted'])
                    $file['completed'] = true;
                if ($file['wanted'] && $par->MoveReady && !$file['completed'])
                {
                    continue;
                }
                foreach ([NormalizeFolder($downloadDir), $incomplete_dir] as $dir)
                {
                    foreach ([$file['name'], $file['name'] . ".part"] as $fname)
                    {
                        if (str_ends_with($fname, '.part') && !$movePartials && !$forceThis)
                            continue;
                        $oldFile = $dir . $fname;
                        $newFile = $destinationDir . $file['niceName'];
                        if (isFile($oldFile))
                        {
                            if ($file['wanted'])
                            {
                                if (moveFile($oldFile, $newFile, length: $l) || $par->ignore)
                                {
                                    $Completed[] = $file["id"];
                                    MakeReadable($newFile);
                                    if ($destinationDir != $downloadDir)
                                    {
                                        if (isFile($oldFile) && $oldFile != $newFile)
                                        {
                                            unlink($oldFile);
                                        }
                                    }
                                }
                                else
                                {
                                    cputs("light_red", "Move failed " . preg_replace("@[\\\\/]@", DIRECTORY_SEPARATOR, $oldFile) . PHP_EOL);
                                    // teeoff();
                                    $Failed[] = $file["id"];
                                }
                            }
                            else
                            {
                                cputs("yellow", "remove " . preg_replace("@[\\\\/]@", DIRECTORY_SEPARATOR, $oldFile) . PHP_EOL);
                                $Removed[] = $file["id"];
                                unlink($oldFile);
                            }
                        }
                    }

                }

            }
            if ($Sonarr)
            {
                $sonarr = getSonarr();
                if ($sonarr !== null)
                {
                    $manualImport = isset($torrent->labels) && is_array($torrent->labels) && in_array('manual', $torrent->labels);
                    if ($manualImport)
                    {
                        cprintf("light_green", "Import torrent to Sonarr  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                        $scanDir = $destinationDir;
                        foreach ($new_files as $file)
                        {
                            if (!$file['wanted'])
                                continue;
                            $candidate = $destinationDir . str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $file['niceName']);
                            if (is_file($candidate))
                            {
                                $scanDir = dirname($candidate);
                                break;
                            }
                        }
                        import2Sonarr($sonarr, $scanDir);
                        scriptRemoveIfAllowed($rpc, $torrent->id, false);
                        cprintf("light_green", "Torrent removed after Sonarr import  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    }
                    else
                    {
                        cprintf("yellow", "Sonarr will import it automatically, skip import  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    }
                }
                $rpc->stop($torrent->id);
                // scriptRemoveIfAllowed($rpc, $torrent->id, false);
                // if ($isSergeybo02)
                foreach ($qbHosts as $qbHost)
                    removeQbittorrentDuplicate($TR_TORRENT_HASH, host: $qbHost);
                exit(0);
            }

            if ($par->MoveReady || count($Failed) > 0)
            {
                if ($Import && $par->MoveReady)
                {
                    cprintf("light_green", "Import torrent to Whisparr  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    $scanDirs = [];
                    foreach ($new_files as $file)
                    {
                        if (!$file['wanted'])
                            continue;
                        $candidate = $destinationDir . str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $file['niceName']);
                        if (is_file($candidate))
                        {
                            $scanDirs[dirname($candidate)] = dirname($candidate);
                        }
                    }
                    uasort($scanDirs, function ($a, $b): int
                    {
                        $depth = substr_count(NormalizeFolder($b), DIRECTORY_SEPARATOR) <=> substr_count(NormalizeFolder($a), DIRECTORY_SEPARATOR);
                        if ($depth !== 0)
                        {
                            return $depth;
                        }
                        return strnatcasecmp($a, $b);
                    });
                    $importedCount = 0;
                    foreach ($scanDirs as $scanDir)
                    {
                        $importedCount += import2Whisparr($client, $scanDir, downloadId: $TR_TORRENT_HASH);
                    }
                    if ($importedCount > 0 && is_dir($destinationDir . $TR_TORRENT_NAME))
                    {
                        if ($removeAllFilesAfterScript)
                            CleanUpDirectory($destinationDir . $TR_TORRENT_NAME);
                    }
                }

                if ((count($Completed) > 0) || (count($Removed) > 0))
                {
                    $File = "";
                    foreach ($new_files as $file)
                    {
                        $File = isFile($incomplete_dir . $file['name']) ? $incomplete_dir . $file['name'] :
                            (isFile($incomplete_dir . $file['name'] . ".part") ? $incomplete_dir . $file['name'] . ".part" :
                                (isFile($downloadDir . $file['name']) ? $downloadDir . $file['name'] :
                                    (isFile($downloadDir . $file['name'] . ".part") ? $downloadDir . $file['name'] . ".part" : $File)));
                        if ($File != "")
                            break;
                    }
                    if ($File == "")
                    {
                        $d1 = pathinfo($downloadDir . $new_files[0]['name'], PATHINFO_DIRNAME) . DIRECTORY_SEPARATOR;
                        if (!is_dir($d1))
                            mk_dir($d1, recursive: true);
                        touch($downloadDir . $new_files[0]['name'] . ".part");
                    }
                    // print_r($Completed);
                    if (count($Completed) > 0)
                    {
                        $arguments = array();
                        $arguments['files-unwanted'] = $Completed;
                        $rpc->set($torrent->id, $arguments);
                    }
                    // sleep(5);
                    cputs("light_blue", "Verify torrent" . PHP_EOL);
                    $rpc->verify($torrent->id);
                    if (isset($torrent->labels) && is_array($torrent->labels))
                    {
                        $labels = array_filter($torrent->labels, function ($value) use ($par)
                        {
                            return $value != "Seed";
                        });
                        $rpc->set($torrent->id, ["labels" => $labels]);
                    }
                    if (!($Clips || $TVShow || $Movies || $Whisparr))
                    {
                        continue;
                    }
                }
            }
            else
            {
                cputs("light_blue", "Remove torrent [$TR_TORRENT_ID] $torrent->name" . PHP_EOL);
                scriptRemoveIfAllowed($rpc, $TR_TORRENT_ID, (bool) $removeDir);
                // if ($isSergeybo02)
                foreach ($qbHosts as $qbHost)
                    removeQbittorrentDuplicate($TR_TORRENT_HASH, host: $qbHost);
                if ($Clips || $DVD)
                {
                    echo "Make local list file" . PHP_EOL;
                    $content = "";
                    $lines = array();
                    $line = sprintf("%-101s ", substr($TR_TORRENT_HASH . " " . getNice($torrent->name), 0, 101)) . date("d.m.Y H:i:s") . " $TR_TORRENT_COMMENT";
                    $key = substr($line, 108, 4) . substr($line, 105, 2) . substr($line, 102, 2) . substr($line, 113, 2) . substr($line, 116, 2) . substr($line, 119, 2) . substr($line, 0, 40);
                    $lines[$key] = trim($line);
                    if (isWindows())
                    {
                        $patterns = "@(.*)Incomplete[\\\/]*$@";
                        if ($DVD)
                            $replacements = "$1DVD";
                        else
                            $replacements = "$1Clips";

                        $filename = preg_replace($patterns, $replacements, $downloadDir) . DIRECTORY_SEPARATOR . "list.4.txt";

                    }
                    else
                        $filename = "/share/Video/RSS/list.txt";
                    if (isFile($filename))
                    {
                        $content .= file_get_contents($filename);
                        setReadOnly($filename, true);
                    }
                    foreach (explode("\n", $content) as $line)
                    {
                        if (($line == "\r") || ($line == ""))
                        {
                            //$lines["9999"]=$line;
                            continue;
                        }
                        $key = substr($line, 108, 4) . substr($line, 105, 2) . substr($line, 102, 2) . substr($line, 113, 2) . substr($line, 116, 2) . substr($line, 119, 2) . substr($line, 0, 40);
                        $lines[$key] = trim($line);//substr($line, 0, 121);
                    }
                    ksort($lines, SORT_STRING);

                    $fp = @fopen($filename, 'w');
                    if ($fp !== false)
                    {
                        foreach ($lines as $line)
                        {
                            fputs($fp, $line . PHP_EOL);
                        }
                        fclose($fp);
                    }

                    //setReadOnly($downloadDir.DIRECTORY_SEPARATOR."list.4.txt");
                    $content = "";
                    // Шары для доставки: креды — в _netrc (machine *-smb),
                    // mapDrive() сам берёт их оттуда и не мапит занятые буквы.
                    mapDrive("v", "\\\\qnap\\Video");
                    mapDrive("Z", "\\\\samsung-pc\\Z$");
                    mapDrive("X", "\\\\samsung-pc\\X$");
                    mapDrive("T", "\\\\TrueNas\\Video");

                    if (1 == 0 && (is_dir("v:\\rss\\") || is_dir("t:\\rss\\")))
                    {
                        echo "Make list file" . PHP_EOL;
                        $list = array(
                            $filename,
                            "\\\\samsung-pc\\z$\\Video\\Clips\\list.4.txt",
                            "\\\\samsung-pc\\x$\\Video\\Clips\\list.4.txt",
                            // "\\\\huawei\\h$\\Clips\\list.4.txt",
                            // "\\\\huawei\\h$\\DVD\\list.4.txt",
                            "\\\\sergeybo02\c$\Users\SERGEYBO\Videos\\Clips\\list.4.txt",
                            "\\\\sergeybo02\c$\Users\SERGEYBO\Videos\\DVD\\list.4.txt",
                            "v:\\rss\\list.4.txt",
                            "v:\\rss\\list.txt",
                            "t:\\rss\\list.txt"
                        );
                        if (array_key_exists('HOME', $_ENV) && is_dir($_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps"))
                        {
                            $list[] = $_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps/list.4.txt";
                        }
                        foreach ($list as $l)
                        {
                            if (isFile($l))
                                $content .= file_get_contents($l);
                        }
                        foreach (explode("\n", $content) as $line)
                        {
                            if (($line == "\r") || ($line == ""))
                            {
                                continue;
                            }
                            $key = substr($line, 108, 4) . substr($line, 105, 2) . substr($line, 102, 2) . substr($line, 113, 2) . substr($line, 116, 2) . substr($line, 119, 2) . substr($line, 0, 40);
                            $lines[$key] = trim($line);//substr($line, 0, 121);
                        }
                        ksort($lines, SORT_STRING);
                        $filename = "v:\\rss\\list.4.txt";
                        $fp = @fopen($filename, 'w');
                        if ($fp !== false)
                        {
                            foreach ($lines as $line)
                            {
                                fputs($fp, $line . PHP_EOL);
                            }
                            fclose($fp);
                        }

                        if (array_key_exists('HOME', $_ENV) && is_dir($_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps"))
                        {
                            $filename = $_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps/list.4.txt";
                            $fp = @fopen($filename, 'w');
                            if ($fp !== false)
                            {
                                foreach ($lines as $line)
                                {
                                    fputs($fp, $line . PHP_EOL);
                                }
                                fclose($fp);
                            }
                            $filename = $_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps/list.txt";
                            $fp = @fopen($filename, 'w');
                            if ($fp !== false)
                            {
                                foreach ($lines as $line)
                                {
                                    fputs($fp, $line . PHP_EOL);
                                }
                                fclose($fp);
                            }
                        }



                        //                    file_put_contents("v:\\rss\\list.4.txt",implode(PHP_EOL,$lines));
                    }
                    else
                    {
                        echo "Make list file 2" . PHP_EOL;
                        $list = array(
                            $filename,
                            "\\\\samsung-pc\\z$\\Video\\Clips\\list.4.txt",
                            "\\\\samsung-pc\\x$\\Video\\Clips\\list.4.txt",
                            // "\\\\huawei\\h$\\Clips\\list.4.txt",
                            // "\\\\huawei\\h$\\DVD\\list.4.txt",
                            "\\\\sergeybo02\c$\Users\SERGEYBO\Videos\\Clips\\list.4.txt",
                            "\\\\sergeybo02\c$\Users\SERGEYBO\Videos\\DVD\\list.4.txt",
                        );
                        if (array_key_exists('HOME', $_ENV) && is_dir($_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps"))
                        {
                            $list[] = $_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps/list.4.txt";
                        }
                        foreach ($list as $l)
                        {
                            if (isFile($l))
                                $content .= file_get_contents($l);
                        }
                        foreach (explode("\n", $content) as $line)
                        {
                            if (($line == "\r") || ($line == ""))
                            {
                                continue;
                            }
                            $key = substr($line, 108, 4) . substr($line, 105, 2) . substr($line, 102, 2) . substr($line, 113, 2) . substr($line, 116, 2) . substr($line, 119, 2) . substr($line, 0, 40);
                            $lines[$key] = substr($line, 0, 121);
                        }
                        ksort($lines, SORT_STRING);

                        if (array_key_exists('HOME', $_ENV) && is_dir($_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps"))
                        {
                            $filename = $_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps/list.4.txt";
                            $fp = @fopen($filename, 'w');
                            if ($fp !== false)
                            {
                                foreach ($lines as $line)
                                {
                                    fputs($fp, $line . PHP_EOL);
                                }
                                fclose($fp);
                            }
                            $filename = $_ENV['HOME'] . "/AppData/Local/Microsoft/WindowsApps/list.txt";
                            $fp = @fopen($filename, 'w');
                            if ($fp !== false)
                            {
                                foreach ($lines as $line)
                                {
                                    fputs($fp, $line . PHP_EOL);
                                }
                                fclose($fp);
                            }
                        }
                    }
                }
                if ($incomplete_dir != $destinationDir && is_dir($incomplete_dir . $torrent->name))
                {
                    if ($removeAllFilesAfterScript)
                        CleanUpDirectory($incomplete_dir . $torrent->name);
                }

                if ($downloadDir != $destinationDir && is_dir($downloadDir . $torrent->name))
                {
                    if ($removeAllFilesAfterScript)
                        CleanUpDirectory($downloadDir . $torrent->name);
                }
            }
            if ($status != TransmissionRPC::TR_STATUS_STOPPED && $par->MoveReady)
            {
                $rpc->start($torrent->id);
                cputs("light_blue", "Start torrent" . PHP_EOL);
            }

            if ($par->make_txt || $par->make_jpg || $par->make_hash)
            {
                $dirs[$destinationDir] = $destinationDir;
            }
            //$destinationDir = $downloadDir;
            if ($Incomplete)
            {
                $patterns = array();
                $replacements = array();
                $patterns[] = "@(.*)Incomplete[\\\/]*$@";
                if ($DVD)
                    $replacements[] = "$1DVD";
                elseif ($Movies)
                    $replacements[] = "$1Movies";
                elseif ($TVShow)
                    $replacements[] = "$1TV-Shows";
                else
                    $replacements[] = "$1Clips";

                $destinationDir = preg_replace($patterns, $replacements, $downloadDir);
                // $fulldirs[$destinationDir] = $destinationDir;
            }
        }
    }
    if ((count($dirs) > 0) && ($TVShow || $Movies || $Clips || $DVD || $Whisparr))
    {
        $descriptions = array();
        if ($par->description && ($par->make_txt || $par->make_jpg || $par->make_hash))
        {
            DescriptionMedia($descriptions, array_keys($dirs), $par->make_txt, $par->make_jpg, $par->make_hash);
            foreach ($descriptions as $description)
            {
                $description->flush();
            }
        }
    }
    if ((count($fulldirs) > 0) && $par->Info)
    {
        $descriptions = array();
        if ($par->description)
        {
            DescriptionMedia($descriptions, array_keys($fulldirs));
            foreach ($descriptions as $description)
            {
                $description->flush();
            }
        }
    }
}
if ($Imported)
{
    cputs("light_green", "Check downloaded episodes in Whisparr" . PHP_EOL);
    $client = getWhisparr();
    checkDownloadedEpisodes($client);
}
if ($rpc->connected && $torrents_found == 0)
{
    $ids = count($par->ids) > 0 ? implode(", ", $par->ids) : "(none)";
    cputs("yellow", "No torrents found on " . $rpc->url . " for ids: $ids" . PHP_EOL);
}
cputs("light_green", PHP_EOL . "Done" . PHP_EOL);
exit(0);