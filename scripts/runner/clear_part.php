#!/usr/bin/env php
<?php
declare(strict_types=1);

require_once __DIR__ . '/runner_bootstrap.php';

function clearPartFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'torrent');

    $torrents = runnerTorrents($payload);
    if ($torrents === [])
        runnerFail('no torrents selected');

    $parameters = runnerParameters($payload);
    $keepWanted = ($parameters['keep_wanted'] ?? false) === true;
    $rpc = runnerRpc($payload);

    foreach ($torrents as $torrent)
        ClearPartTorrent($rpc, runnerTorrentId($torrent), $keepWanted);
}

function clearPartFromCli(): void
{
    runnerMapArgvAliases(['--wanted' => '--keep-wanted']);

    $defaultSite = file_exists('server.txt') ? trim(file_get_contents('server.txt')) : 'amdocs';
    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, $defaultSite)
        ->addParameter('ids', 't|id', 'set torrent ids', ParameterType::Number, ParameterCount::Multiple, Mandatory::Mandatory)
        ->addParameter('keep_wanted', 'w|keep-wanted', 'keep wanted files', ParameterType::Boolean, default: false)
        ->Process();

    $server = GetServer($par->site);
    if ($server === false)
        runnerFail('no server available');

    $rpc = new TransmissionRPC($server, use_netrc: true);
    if (!$rpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    foreach ($par->ids as $id)
        ClearPartTorrent($rpc, $id, $par->keep_wanted);
}

$payload = runnerTryReadPayload();
if ($payload === null)
    clearPartFromCli();
else
    clearPartFromPayload($payload);
