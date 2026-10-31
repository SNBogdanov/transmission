#!/usr/bin/env php
<?php
declare(strict_types=1);

require_once __DIR__ . '/runner_bootstrap.php';

function makeNiceFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'torrent');

    $torrents = runnerTorrents($payload);
    if ($torrents === [])
        runnerFail('no torrents selected');

    $parameters = runnerParameters($payload);
    $extraDir = ($parameters['dir'] ?? false) === true;
    $rpc = runnerRpc($payload);

    foreach ($torrents as $torrent)
        MakeNiceTorrent($rpc, runnerTorrentId($torrent), extraDir: $extraDir);
}

function makeNiceFromCli(): void
{
    $defaultSite = file_exists('server.txt') ? trim(file_get_contents('server.txt')) : 'amdocs';
    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, $defaultSite)
        ->addParameter('ids', 't|id', 'set torrent ids', ParameterType::Number, ParameterCount::Multiple, Mandatory::Mandatory)
        ->addParameter('dir', '|dir', 'create nice directories', ParameterType::Boolean)
        ->Process();

    $server = GetServer($par->site);
    if ($server === false)
        runnerFail('no server available');

    $rpc = new TransmissionRPC($server, use_netrc: true);
    if (!$rpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    foreach ($par->ids as $id)
        MakeNiceTorrent($rpc, $id, extraDir: $par->dir);
}

$payload = runnerTryReadPayload();
if ($payload === null)
    makeNiceFromCli();
else
    makeNiceFromPayload($payload);
