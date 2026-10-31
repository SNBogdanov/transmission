#!/usr/bin/env php
<?php
declare(strict_types=1);

global $use_lock;
$use_lock = true;

require_once __DIR__ . '/runner_bootstrap.php';
require_once 'Torrent.php';

global $Names;
$Names->setDirectory(__DIR__);

function copyTorrents(TransmissionRPC $localRpc, TransmissionRPC $remoteRpc, array $ids, string $path, bool $run, string $localLabel, string $remoteLabel): void
{
    $fields = ['id', 'name', 'torrentFile', 'labels', 'wanted', 'haveValid', 'totalSize'];
    $result = $localRpc->get($ids, $fields);
    if (TransmissionRPC::check_result($result, 'torrents'))
    {
        foreach ($result->arguments->torrents as $torrent)
        {
            if ($torrent->haveValid == $torrent->totalSize)
            {
                printf("Skip %s because it has ben completed%s", $torrent->name, PHP_EOL);
                continue;
            }
            if (!is_file($torrent->torrentFile))
                continue;

            $content = file_get_contents($torrent->torrentFile);
            printf("File=%s Name=%s %s", preg_replace("@[/\\\\]@", DIRECTORY_SEPARATOR, $torrent->torrentFile), $torrent->name, PHP_EOL);

            $t = new Torrent($content);
            $labels = is_array($torrent->labels) ? $torrent->labels : [];
            $wanted = is_array($torrent->wanted) ? $torrent->wanted : [];
            $arr = [];
            $arr['labels'] = $labels;
            $arr['files-unwanted'] = array_keys(array_filter($wanted, function ($w)
            {
                return $w == 0;
            }));
            $arr['files-wanted'] = array_keys(array_filter($wanted, function ($w)
            {
                return $w == 1;
            }));

            $destination = $path;
            if ($destination === '' && array_search('DVD', $labels) !== false)
                $destination = preg_replace("/Clips$/", "DVD", $remoteRpc->download_dir);
            if (!$run)
                $arr['paused'] = true;

            $arr['sequentialDownload'] = true;
            $arr['sequential_download'] = true;
            $added = $remoteRpc->add_metainfo($content, $destination, $arr);
            if (!TransmissionRPC::check_result($added))
                continue;

            $addedTorrent = $added->arguments->torrent_added ?? ($added->arguments->torrent_duplicate ?? null);
            if (!is_object($addedTorrent))
                continue;
            if (isset($added->arguments->torrent_duplicate))
                printf("Torrent was added before%s", PHP_EOL);

            if ($addedTorrent->name != $torrent->name)
                $remoteRpc->rename($addedTorrent->id, $addedTorrent->name, $torrent->name);
            priority($remoteRpc, $addedTorrent->id, orderType::byID, true);
            $remoteRpc->verify($addedTorrent->id);
        }
    }

    $fields = ['id', 'name', 'hashString'];
    $local = $localRpc->get(fields: $fields);
    $remote = $remoteRpc->get(fields: $fields);
    $localTorrents = [];
    foreach ($local->arguments->torrents as $torrent)
        $localTorrents[$torrent->hashString] = $torrent;

    foreach ($remote->arguments->torrents as $torrent)
    {
        if (!array_key_exists($torrent->hashString, $localTorrents))
            continue;

        printf("Remove from %s %s%s", $localLabel, $torrent->name, PHP_EOL);
        $localRpc->remove($localTorrents[$torrent->hashString]->id, true);
    }
}

function copyToFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'torrent');

    $torrents = runnerTorrents($payload);
    if ($torrents === [])
        runnerFail('no torrents selected');

    $parameters = runnerParameters($payload);
    $target = $parameters['target'] ?? null;
    if (!is_string($target) || $target === '')
        runnerFail('parameter target is required');

    $path = $parameters['path'] ?? '';
    if (!is_string($path))
        runnerFail('parameter path must be a string');

    $host = runnerExternalHost($payload, $target);
    $netrcMachine = isset($host['netrc_machine']) && is_string($host['netrc_machine']) && $host['netrc_machine'] !== ''
        ? $host['netrc_machine']
        : $target;
    $localRpc = runnerRpc($payload);
    $remoteRpc = runnerRpcFromEndpoint($host['rpc_url'], $payload['rpc']['netrc'] ?? null, $netrcMachine);

    copyTorrents(
        $localRpc,
        $remoteRpc,
        runnerSelectedTorrentIds($payload),
        $path,
        ($parameters['run'] ?? false) === true,
        (string) ($payload['server']['host'] ?? 'localhost'),
        (string) ($host['name'] ?? $target)
    );
}

function copyToFromCli(): void
{
    runnerMapArgvAliases(['--remote' => '--target']);

    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server from which to copy', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, 'localhost')
        ->addParameter('target', 'e|target', 'server where to copy', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory)
        ->addParameter('ids', 't|id', 'set torrent ids', ParameterType::Number, ParameterCount::Multiple)
        ->addParameter('path', 'p|path', 'destination path', ParameterType::String, ParameterCount::Single, Mandatory::Optional)
        ->addParameter('run', 'r|run', 'start download', ParameterType::Boolean)
        ->Process();

    $remote = GetServer($par->target);
    if ($remote === false)
        runnerFail('no server available');

    $local = GetServer($par->site);
    if ($local === false)
        runnerFail('no server available');

    $localRpc = new TransmissionRPC($local, use_netrc: true);
    $remoteRpc = new TransmissionRPC($remote, use_netrc: true);
    if (!$localRpc->connected || !$remoteRpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    copyTorrents($localRpc, $remoteRpc, $par->ids, (string) ($par->path ?? ''), $par->run, (string) $par->site, (string) $par->target);
}

$payload = runnerTryReadPayload();
if ($payload === null)
    copyToFromCli();
else
    copyToFromPayload($payload);
