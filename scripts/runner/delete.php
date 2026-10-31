#!/usr/bin/env php
<?php
declare(strict_types=1);

require_once __DIR__ . '/runner_bootstrap.php';

function checkOtherFiles(array $dirs, array $files, string $fileName): bool
{
    foreach ($files as $file)
    {
        $path = str_replace('/', DIRECTORY_SEPARATOR, $file->name);
        foreach ($dirs as $dir)
        {
            if ($fileName == ($dir . DIRECTORY_SEPARATOR . $path))
                continue;
            if ($fileName == ($dir . DIRECTORY_SEPARATOR . $path . ".part"))
                continue;
            if (is_file($dir . DIRECTORY_SEPARATOR . $path) || is_file($dir . DIRECTORY_SEPARATOR . $path . ".part"))
                return true;
        }
    }

    return false;
}

function deleteSelectedFiles(TransmissionRPC $rpc, object $torrent, array $requestedFiles): void
{
    $dirs = [];
    if ($rpc->incomplete_dir_enabled)
        $dirs[] = $rpc->incomplete_dir;
    if (!isset($torrent->files))
        return;
    $dirs[] = $torrent->downloadDir;

    $found = false;
    $filesUnwanted = [];
    foreach ($requestedFiles as $requestedFile)
    {
        if (!is_numeric($requestedFile) || !array_key_exists((int) $requestedFile, $torrent->files))
            continue;

        $index = (int) $requestedFile;
        $name = str_replace('/', DIRECTORY_SEPARATOR, $torrent->files[$index]->name);
        printf("%s" . PHP_EOL, $name);
        foreach ($dirs as $dir)
        {
            $fileName = $dir . DIRECTORY_SEPARATOR . $name;
            $partName = $fileName . ".part";
            if (!is_file($fileName) && !is_file($partName))
                continue;

            if (!$found)
            {
                $rpc->stop($torrent->id);
                sleep(2);
            }

            try
            {
                if (checkOtherFiles($dirs, $torrent->files, $fileName))
                {
                    if (is_file($fileName))
                        unlink($fileName);
                    if (is_file($partName))
                        unlink($partName);
                }
                else
                {
                    file_put_contents($fileName, "");
                }
            }
            catch (Throwable $e)
            {
            }

            $found = true;
        }

        $filesUnwanted[] = $index;
    }

    for ($i = 0; $i < count($torrent->files); $i++)
    {
        foreach ($requestedFiles as $requestedFile)
        {
            if (is_numeric($requestedFile))
                continue;
            if (!str_ends_with(preg_replace("/\.part$/", "", (string) $requestedFile), str_replace("/", DIRECTORY_SEPARATOR, $torrent->files[$i]->name)))
                continue;

            printf("%s" . PHP_EOL, $torrent->files[$i]->name);
            $filesUnwanted[] = $i;
            $requestedPath = (string) $requestedFile;
            if (is_file($requestedPath))
            {
                printf("delete %s%s", $requestedPath, PHP_EOL);
                if (!$found)
                {
                    $rpc->stop($torrent->id);
                    sleep(2);
                }
                try
                {
                    if (checkOtherFiles($dirs, $torrent->files, $requestedPath))
                        unlink($requestedPath);
                    else
                        file_put_contents($requestedPath, "");
                }
                catch (Throwable $e)
                {
                }
                $found = true;
            }
            elseif (is_file($requestedPath . ".part"))
            {
                printf("delete %s.part%s", $requestedPath, PHP_EOL);
                if (!$found)
                {
                    $rpc->stop($torrent->id);
                    sleep(2);
                }
                try
                {
                    if (checkOtherFiles($dirs, $torrent->files, $requestedPath))
                        unlink($requestedPath . ".part");
                    else
                        file_put_contents($requestedPath . ".part", "");
                }
                catch (Throwable $e)
                {
                }
                $found = true;
            }
        }
    }

    if (count($torrent->files) > 1)
        $rpc->set($torrent->id, ['files-unwanted' => $filesUnwanted]);

    if ($found)
    {
        $rpc->verify($torrent->id);
        if (isset($torrent->status) && ($torrent->status != 0))
            $rpc->start($torrent->id);
    }
}

function deleteFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'file');

    $selected = $payload['files'] ?? null;
    if (!is_array($selected))
        runnerFail('payload files are missing');

    $byTorrent = [];
    foreach ($selected as $file)
    {
        if (!is_array($file) || ($file['selected'] ?? false) !== true)
            continue;

        $torrentId = $file['torrent_id'] ?? null;
        $fileIndex = $file['file_index'] ?? null;
        if (!is_int($torrentId) || !is_int($fileIndex))
            runnerFail('selected file has no torrent_id or file_index');

        $byTorrent[$torrentId][$fileIndex] = $fileIndex;
    }

    if ($byTorrent === [])
        runnerFail('no files selected');

    $rpc = runnerRpc($payload);
    $fields = ['id', 'name', 'files', 'status', 'downloadDir'];
    foreach ($byTorrent as $torrentId => $indices)
    {
        $result = $rpc->get($torrentId, $fields);
        if (!TransmissionRPC::check_result($result, 'torrents') || empty($result->arguments->torrents))
            continue;

        deleteSelectedFiles($rpc, $result->arguments->torrents[0], array_values($indices));
    }
}

function deleteFromCli(): void
{
    $defaultSite = file_exists('server.txt') ? trim(file_get_contents('server.txt')) : 'amdocs';
    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, $defaultSite)
        ->addParameter('ids', 't|id', 'set torrent ids', ParameterType::Number, ParameterCount::Multiple, Mandatory::Mandatory)
        ->addParameter('files', 'f|file', 'list of files', ParameterType::String, ParameterCount::Multiple, Mandatory::Mandatory)
        ->Process();

    $server = GetServer($par->site);
    if ($server === false)
        runnerFail('no server available');

    $rpc = new TransmissionRPC($server, use_netrc: true);
    if (!$rpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    $fields = ['id', 'name', 'files', 'status', 'downloadDir'];
    $rpc->getAndProcess($par->ids, $fields, function ($torrent) use ($rpc, $par): void
    {
        deleteSelectedFiles($rpc, $torrent, $par->files);
    });
}

$payload = runnerTryReadPayload();
if ($payload === null)
    deleteFromCli();
else
    deleteFromPayload($payload);

sleep(3);
