#!/opt/bin/php-cli
<?php
global $use_lock;
$use_lock = true;
require_once __DIR__ . '/runner_bootstrap.php';
require_once 'WhisparrAPIClient.php';

$payload = runnerTryReadPayload();
$TR_TORRENT_ID = getenv('TR_TORRENT_ID');
$TR_TORRENT_NAME = getenv('TR_TORRENT_NAME');
$TR_TORRENT_DIR = getenv('TR_TORRENT_DIR');
$TR_TORRENT_HASH = getenv('TR_TORRENT_HASH');
$par = new Parameters();
$par->addParameter("site", "s|server", "set server", ParameterType::String, ParameterCount::Multiple, mandatory: Mandatory::Mandatory, default: ["localhost"])
    ->addParameter("ids", "t|id", "set torrent id", ParameterType::Number, ParameterCount::Multiple, mandatory: Mandatory::Optional, default: ($TR_TORRENT_ID !== false) ? array($TR_TORRENT_ID) : array())
    ->addParameter("hash", "|hash", "set torrent hash", ParameterType::String, ParameterCount::Multiple, Mandatory::Optional, ($TR_TORRENT_HASH !== false) ? [$TR_TORRENT_HASH] : [])
    ->addParameter("name", "n|name", "set torrent name", ParameterType::String, ParameterCount::Single, Mandatory::Optional, ($TR_TORRENT_NAME !== false) ? $TR_TORRENT_NAME : "")
    ->addParameter("makeThumb", "h|thumb", "make thumb", ParameterType::Boolean, default: false)
    ->addParameter("rename-temp", "r|rename-temp", "Rename temporary files (.part / .!qB)", ParameterType::Boolean, ParameterCount::Single, Mandatory::Optional, true)
    ->addParameter("qb-host", "|qb-host", "qBittorrent host", ParameterType::String, ParameterCount::Single, Mandatory::Optional, "localhost")
    ->Process();

if ($payload !== null)
{
    runnerRequireContext($payload, 'torrent');
    $payloadTorrents = runnerTorrents($payload);
    if ($payloadTorrents === [])
        runnerFail('no torrents selected');

    $parameters = runnerParameters($payload);
    $par->ids = runnerSelectedTorrentIds($payload);
    $par->hash = array_map('runnerTorrentHash', $payloadTorrents);
    $par->name = '';
    $serverHost = trim((string) ($payload['server']['host'] ?? ''));
    $par->site = [$serverHost !== '' ? $serverHost : gethostname()];
    $par->makeThumb = ($parameters['make_thumb'] ?? false) === true;
    $par->{'rename-temp'} = ($parameters['rename_temp'] ?? true) === true;
    $par->{'qb-host'} = (string) ($parameters['qb_host'] ?? 'localhost');
    $rpc = runnerRpc($payload);
}
else
{
    if (count($par->ids) == 0 && count($par->hash) == 0 && empty(trim((string) $par->name)))
    {
        cputs("light_red", "No Torrent set" . PHP_EOL);
        exit(1);
    }

    $rpc = getTransmission($par->site[0]);
    if ($rpc === null)
    {
        cputs("light_red", "No server available" . PHP_EOL);
        exit(1);
    }
}

// When the caller explicitly selects torrents by id (CLI, runner payload or
// TR_TORRENT_ID env), do not let the hash route the request through the
// qBittorrent duplicate check. An explicit --hash still takes precedence.
$explicitIdArg = false;
$explicitHashArg = false;
if (isset($argv) && is_array($argv))
{
    foreach (array_slice($argv, 1) as $arg)
    {
        if ($arg === '-t' || $arg === '--id' || str_starts_with($arg, '--id=') || preg_match('/^-t\d+$/', $arg) === 1)
            $explicitIdArg = true;
        if ($arg === '--hash' || str_starts_with($arg, '--hash='))
            $explicitHashArg = true;
    }
}
$idProvided = $payload !== null || $explicitIdArg;
if ($TR_TORRENT_ID !== false && count((array) $par->ids) > 0)
    $idProvided = true;
if ($idProvided && !$explicitHashArg)
{
    $par->hash = [];
}

$par1 = clone $par;
$finished = 0;

// Transmission host name for path mapping (pathmap.php keys).
$trHost = strtolower(trim((string) $par->site[0]));
if ($trHost === 'amdocs')
{
    $trHost = 'sergeybo02';
}
elseif ($trHost === 'localhost')
{
    $trHost = strtolower(trim((string) gethostname()));
}

// Map a Transmission (remote) path to a local path on this machine.
$toLocal = function (string $p) use ($trHost): string
{
    if ($p === '')
    {
        return $p;
    }
    return NormalizeFolder(mapRemoteToLocalPath($p, [], $trHost, null));
};

// Ensure a remote Transmission path has a trailing separator in its own style.
$remoteDir = function (string $p): string
{
    if ($p === '')
    {
        return $p;
    }
    $sep = (str_contains($p, '/') && !str_contains($p, '\\')) ? '/' : '\\';
    return rtrim($p, '/\\') . $sep;
};

// // Используйте API ключ (рекомендуется) или логин/пароль
$client = getWhisparr();

$res = $client->getQueue(asObjects: true);
$total = $res->totalRecords;
cprintf("light_green", "Total items in queue: %d\n", $total);
$queue = [];
for ($i = 0; $i < $total; $i += 100)
{
    $res = $client->getQueue($i / 100 + 1, 100, asObjects: true);
    $queue = array_merge($queue, $res->records);
}

$downloads = [];

// Build the list of connected Transmission RPC clients.
$rpcClients = [];
if ($payload !== null)
{
    $rpcClients[$trHost] = $rpc;
}
else
{
    foreach ($par->site as $siteName)
    {
        $rpcClient = getTransmission($siteName);
        if ($rpcClient !== null)
        {
            $rpcClients[$siteName] = $rpcClient;
        }
    }
}

if ($rpc->connected)
{
    cputs("light_green", "Connected to " . $rpc->url . PHP_EOL);
    $incomplete_dir = $rpc->incomplete_dir_enabled ? $remoteDir($rpc->incomplete_dir) : "";
    $fields = array('id', 'name', 'status', 'downloadDir', 'labels', 'hashString', 'comment');
    $hashes = array_values(array_filter(array_map('trim', (array) $par->hash)));
    $name = trim((string) $par->name);
    $qb = getQbittorrent($par->{'qb-host'});
    if (!empty($name) && count($hashes) == 0)
    {
        $hash = findTorrentHashByName($name, $rpcClients, $qb);
        if ($hash === null || $hash === '')
        {
            cputs("light_red", "Torrent not found by name: {$name}" . PHP_EOL);
            exit(1);
        }
        cprintf("light_green", "Found torrent by name \"%s\": %s" . PHP_EOL, $name, $hash);
        $hashes = [$hash];
    }

    $transmissionHashes = [];
    foreach ($hashes as $hash)
    {
        $qbTorrents = $qb !== null ? $qb->getTorrentList(['hashes' => $hash]) : [];
        if (!empty($qbTorrents))
        {
            cprintf("yellow", "Processing torrent in qBittorrent (hash: %s)" . PHP_EOL, $hash);
            if (!importQbittorrent($hash, $par->{'rename-temp'}, $par->{'qb-host'}, [], $qb))
            {
                cprintf("light_red", "qBittorrent import failed, falling back to Transmission: %s" . PHP_EOL, $hash);
                $transmissionHashes[] = $hash;
                continue;
            }
            removeTorrentFromTransmission($hash, array_keys($rpcClients));
        }
        else
        {
            cprintf("yellow", "Torrent not found in qBittorrent, falling back to Transmission (hash: %s)" . PHP_EOL, $hash);
            $transmissionHashes[] = $hash;
        }
    }
    $ids = !empty($hashes) ? $transmissionHashes : $par->ids;
    if (count($ids) > 0)
    {
        $extPriority = ['.part', '.!qB'];
        $torrents = [];
        $rpc->getAndProcess($ids, $fields, processor: function ($torrent) use (&$torrents): void
        {
            $torrents[] = $torrent;
        });
        if (count($torrents) == 0)
        {
            cputs("light_red", "No torrent found with specified id(s)" . PHP_EOL);
            exit(1);
        }

        $imports = [];       // список директорий/файлов для импорта в Whisparr
        $removeList = [];    // список торрентов для удаления

        foreach ($torrents as $torrent)
        {
            $par = clone $par1;
            $Whisparr = false;
            $TR_TORRENT_ID = $torrent->id;
            $TR_TORRENT_HASH = $torrent->hashString;
            $TR_TORRENT_DIR = $torrent->downloadDir;
            $TR_TORRENT_NAME = $torrent->name;
            $download = array_filter($queue, function ($item) use ($torrent)
            {
                return strcasecmp($item->downloadId, $torrent->hashString) == 0;
            });

            if (!$rpc->incomplete_dir_enabled)
            {
                $incomplete_dir = $TR_TORRENT_DIR;
            }
            $destinationDir = $downloadDir = $TR_TORRENT_DIR;
            if (!is_null($torrent->labels) && is_array($torrent->labels) && in_array('Whisparr', $torrent->labels))
            {
                $Whisparr = true;
            }
            $Whisparr = $Whisparr || preg_match('@.*whisparr[\\\\/]?$@i', $destinationDir) == 1;
            if (!$Whisparr)
            {
                cprintf("yellow", "Torrent [ %4d ] %s is not in whisparr folder, skipping" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                continue;
            }
            $Incomplete = preg_match('@.*Incomplete[\\\/]*$@', $destinationDir) == 1;
            if ($Incomplete)
            {
                $sep = (str_contains($destinationDir, '/') && !str_contains($destinationDir, '\\')) ? '/' : '\\';
                $destinationDir = $remoteDir(preg_replace('@(.*Incomplete)[\\\/]*$@', '$1' . $sep . 'Whisparr', $destinationDir));
            }

            // Local equivalents of the Transmission paths (for filesystem operations).
            $localIncompleteDir = $toLocal($incomplete_dir);
            $localDestinationDir = $toLocal($destinationDir);
            $localDownloadDir = $toLocal($downloadDir);

            if (isset($torrent->status))
                $status = $torrent->status;
            else
                $status = TransmissionRPC::TR_STATUS_STOPPED;
            $fields = array('id', 'files', 'fileStats', 'downloadDir', 'name');
            $res1 = $rpc->getAndProcess($torrent->id, $fields, processor: function ($torrent1) use (&$torrent, &$par, $par1, $queue, $rpc, $qb, $extPriority, $incomplete_dir, &$destinationDir, &$downloadDir, $toLocal, $remoteDir, &$localIncompleteDir, &$localDestinationDir, &$localDownloadDir, $status, &$TR_TORRENT_ID, &$TR_TORRENT_HASH, &$TR_TORRENT_DIR, &$TR_TORRENT_NAME, &$imports, &$removeList, &$fields, &$finished): void
            {
                if (!(isset($torrent1->files) && is_array($torrent1->files)))
                    return;

                $mediaCheckPaths = [
                    $localIncompleteDir . $torrent->name,
                    $localDownloadDir . $torrent->name,
                    $localDestinationDir,
                    $localDestinationDir . $torrent->name,
                ];
                if (!checkDownloadedMediaInfo($mediaCheckPaths, $extPriority))
                {
                    cprintf("yellow", "MediaInfo check failed, skip torrent: %s" . PHP_EOL, $TR_TORRENT_NAME);
                    Logger::warning('MediaInfo check failed, skip torrent: ' . $TR_TORRENT_NAME);
                    return;
                }

                // ===== Фаза 1: заполнение массива директорий, остановка торрентов, перенос =====
                $paths = [];
                // Остановка Transmission
                if ($status != TransmissionRPC::TR_STATUS_STOPPED)
                {
                    cprintf("light_blue", "Stop torrent [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    $rpc->stop($torrent->id);
                    $stopDeadline = time() + 60;
                    while (time() < $stopDeadline)
                    {
                        $stopRes = $rpc->get($torrent->id, ['id', 'status']);
                        $stopStatus = $stopRes->arguments->torrents[0]->status ?? null;
                        if ($stopStatus === TransmissionRPC::TR_STATUS_STOPPED || $stopStatus === null)
                            break;
                        sleep(1);
                    }
                }

                $sourceDirs = array_values(array_unique(array_filter(array_map(
                    static fn($dir): string => rtrim((string) $dir, "/\\"),
                    [$localIncompleteDir, $localDownloadDir]
                ))));
                $destinationRoot = rtrim($localDestinationDir, "/\\") . DIRECTORY_SEPARATOR;
                $moveTorrentFiles = function (array $files) use ($sourceDirs, $destinationRoot, $TR_TORRENT_NAME): bool
                {
                    $ok = true;
                    foreach ($files as $file)
                    {
                        $relative = str_replace(['/', '\\'], DIRECTORY_SEPARATOR, (string) $file->name);
                        $src = null;
                        $srcSuffix = '';
                        foreach ($sourceDirs as $sourceDir)
                        {
                            $base = $sourceDir . DIRECTORY_SEPARATOR . $relative;
                            foreach (['', '.part', '.!qB'] as $suffix)
                            {
                                if (is_file($base . $suffix))
                                {
                                    $src = $base . $suffix;
                                    $srcSuffix = $suffix;
                                    break 2;
                                }
                            }
                        }
                        $dst = $destinationRoot . $relative . $srcSuffix;
                        if ($src === null)
                        {
                            if (is_file($destinationRoot . $relative) || is_file($destinationRoot . $relative . '.part') || is_file($destinationRoot . $relative . '.!qB'))
                                continue;
                            if (preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $relative) !== 1)
                                continue;
                            Logger::warning('Import source file not found', ['torrent' => $TR_TORRENT_NAME, 'file' => $relative]);
                            $ok = false;
                            continue;
                        }
                        if ($src === $dst)
                            continue;
                        if (!moveFile($src, $dst, false))
                        {
                            Logger::warning('Import move failed', ['src' => $src, 'dst' => $dst]);
                            $ok = false;
                        }
                    }
                    return $ok;
                };

                // Перенос из временной папки Transmission в основное назначение
                if (count($torrent1->files) > 1)
                {
                    if (!$moveTorrentFiles($torrent1->files))
                    {
                        cprintf("yellow", "Move failed, skip torrent: %s" . PHP_EOL, $TR_TORRENT_NAME);
                        Logger::warning('Move failed, skip torrent: ' . $TR_TORRENT_NAME);
                        return;
                    }
                    $rpc->move($torrent->id, $destinationDir, false);
                    foreach ($sourceDirs as $sourceDir)
                    {
                        if ($sourceDir !== '')
                            RemoveEmptyDirectories($sourceDir . DIRECTORY_SEPARATOR . $torrent->name);
                    }
                    $paths[] = $destinationRoot . $torrent->name;
                }
                else
                {
                    if (!$moveTorrentFiles($torrent1->files))
                    {
                        cprintf("yellow", "Move failed, skip torrent: %s" . PHP_EOL, $TR_TORRENT_NAME);
                        Logger::warning('Move failed, skip torrent: ' . $TR_TORRENT_NAME);
                        return;
                    }
                    $rpc->move($torrent->id, $destinationDir, false);
                    foreach ($sourceDirs as $sourceDir)
                    {
                        if ($sourceDir !== '')
                            RemoveEmptyDirectories($sourceDir . DIRECTORY_SEPARATOR . $torrent->name);
                    }
                    $paths[] = $destinationRoot . $torrent1->files[0]->name;
                }


                // Остановка qBittorrent + добавление его директории, если отличается
                {
                    $qbTorrent = pauseQbittorrentTorrent($TR_TORRENT_HASH, $par->{'qb-host'}, $qb);
                    if ($qbTorrent !== null)
                    {
                        $qbPath = mapRemoteToLocalPath($qbTorrent['content_path'] ?? $qbTorrent['save_path'] ?? '', [], $par->{'qb-host'}, null);
                        if ($qbPath !== '')
                        {
                            $qbNorm = NormalizeFolder(is_dir($qbPath) ? $qbPath : dirname($qbPath));
                            $found = false;
                            foreach ($paths as $p)
                            {
                                if (strcasecmp(NormalizeFolder(is_dir($p) ? $p : dirname($p)), $qbNorm) === 0)
                                {
                                    $found = true;
                                    break;
                                }
                            }
                            if (!$found)
                            {
                                $paths[] = $qbPath;
                            }
                        }
                    }
                }

                // ===== Фаза 2: переименование временных файлов + массив импорта =====
                foreach ($paths as $path)
                {
                    try
                    {
                        if (is_dir($path))
                        {
                            renameTempFiles($path, $extPriority);
                            $importDir = $path;
                        }
                        else
                        {
                            renameTempFile($path, $extPriority);
                            $importDir = dirname($path);
                        }

                        if (!checkDownloadedMediaInfo($path, $extPriority))
                        {
                            cprintf("yellow", "MediaInfo check failed, skip torrent: %s" . PHP_EOL, $path);
                            Logger::warning('MediaInfo check failed, skip torrent: ' . $path);
                            return;
                        }

                        if (!is_dir($importDir))
                        {
                            cprintf("yellow", "Target directory not found, skip import: %s" . PHP_EOL, $importDir);
                            return;
                        }

                        $imports[] = [
                            'title' => $TR_TORRENT_NAME,
                            'hash' => $TR_TORRENT_HASH,
                            'path' => mapRemoteToLocalPath($importDir, [], null, 'whisparr'),
                        ];
                    }
                    catch (Throwable $e)
                    {
                        cprintf("yellow", "Import path error, skip torrent: %s (%s)" . PHP_EOL, $path, $e->getMessage());
                        Logger::warning('Import path error: ' . $e->getMessage(), ['path' => $path]);
                        return;
                    }
                }
                $removeList[$TR_TORRENT_ID] = ['id' => $TR_TORRENT_ID, 'hash' => $TR_TORRENT_HASH, 'name' => $TR_TORRENT_NAME, 'destinationDir' => $localDestinationDir, 'incompleteDir' => $localIncompleteDir, 'downloadDir' => $localDownloadDir];

                // Свободное место + Telegram
                for ($m = 0, $i = 0; $m <= 0 && $i < 5; $i++, sleep(5))
                {
                    $m = $rpc->freeSpace($destinationDir);
                }
                $free_space = printSize($m);
                echo ($finished++) ? PHP_EOL : "";
                cputs("light_green", "Finish torrent [ $TR_TORRENT_ID ] $TR_TORRENT_NAME at " . date("d.m.Y H:i:s") . PHP_EOL . "Destination => $destinationDir ($free_space)" . PHP_EOL);
                $text = "Finish torrent [ $TR_TORRENT_ID ]" . PHP_EOL . "<b>$TR_TORRENT_NAME</b>" . PHP_EOL . "$destinationDir ($free_space)" . PHP_EOL;
                $files = array_filter($torrent1->files, function ($value): bool
                {
                    return preg_match(AdvancedRecursiveDirectoryIterator::mediaMask, $value->name) === 1;
                });
                if (count($files) == 0 || !$par->makeThumb)
                    TelegramSendMessage($text);
                else
                {
                    $filesArr = [];
                    foreach ($files as $file)
                    {
                        $filesArr[] = ['name' => $file->name, 'completed' => true];
                    }
                    $checkDirs = [$localIncompleteDir, $localDownloadDir, $localDestinationDir];
                    $j = generateThumbnails($filesArr, $checkDirs, true);
                    if (!is_array($j) || count($j) == 0)
                    {
                        cprintf("yellow", "No thumbnails created for torrent %s" . PHP_EOL, $TR_TORRENT_NAME);
                        TelegramSendMessage($text);
                    }
                    else
                    {
                        TelegramSendMessage($text, $j);
                        foreach ($j as $f)
                            unlink($f);
                    }
                }
                if (in_array($status, array(TransmissionRPC::TR_STATUS_CHECK, TransmissionRPC::TR_STATUS_CHECK_WAIT)))
                {
                    cprintf("yellow", "Check is scheduled in torrent  [ %4d ] %s" . PHP_EOL, $TR_TORRENT_ID, $TR_TORRENT_NAME);
                    return;
                }
            });
        }

        // ===== Фаза 3: импорт в Whisparr =====
        $importOk = true;
        foreach ($imports as $import)
        {
            $command = ["name" => "DownloadedEpisodesScan", "path" => $import['path']];
            cprintf("light_green", "Import torrent to Whisparr  %s (%s)" . PHP_EOL, $import['title'], $import['path']);
            $res2 = $client->sendAndWaitCommand($command, true);
            if (is_null($res2) || $res2->result != CommandResult::SUCCESSFUL)
            {
                $msg = PHP_EOL;
                if (!is_null($res2))
                    if (isset($res2->message))
                        $msg = $res2->message . PHP_EOL;
                cputs("light_red", "Failed to send command to Whisparr for torrent " . $import['title'] . ". Reason: " . $msg);
                $importOk = false;
            }
            else
            {
                cputs("light_green", "Command sent successfully to Whisparr for torrent " . $import['title'] . PHP_EOL);
            }
        }

        // ===== Фаза 4: удаление торрентов =====
        foreach ($removeList as $rt)
        {
            removeQbittorrentDuplicate($rt['hash'], false, $par->{'qb-host'}, $qb);

            $rpc->getAndProcess($rt['id'], processor: function ($torrent) use ($rpc, $rt, $importOk): void
            {
                cprintf("yellow", "Removing torrent [ %4d ] %s from Transmission" . PHP_EOL, $rt['id'], $torrent->name);
                $rpc->remove($rt['id'], $importOk);
                if ($importOk)
                {
                    foreach ([$rt['incompleteDir'], $rt['downloadDir']] as $dir)
                    {
                        if ($dir != $rt['destinationDir'] && is_dir($dir . $torrent->name))
                        {
                            CleanUpDirectory($dir . $torrent->name);
                        }
                    }
                }
            });
        }
    }
}
if (count($downloads) > 0 || count($imports ?? []) > 0)
{
    sleep(20);
    cputs("light_green", PHP_EOL . "Check downloaded episodes in Whisparr" . PHP_EOL);
    checkDownloadedEpisodes($client);
}
cputs("light_green", "Done" . PHP_EOL);
exit(0);
