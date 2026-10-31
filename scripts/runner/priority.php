#!/usr/bin/env php
<?php
declare(strict_types=1);

require_once __DIR__ . '/runner_bootstrap.php';

function priorityProcess(TransmissionRPC $rpc, array $torrents, orderType $order, int $maxHigh): void
{
    $print = count($torrents) === 1;

    printf("[  ID] Name" . PHP_EOL);
    foreach ($torrents as $torrent)
    {
        $id = runnerTorrentId($torrent);
        $name = (string) ($torrent['name'] ?? '');
        printf("[%4d] %s" . PHP_EOL, $id, $name);

        if (priority($rpc, $id, $order, $print, max_high: $maxHigh, skip100: true) === false)
            fwrite(STDERR, sprintf("priority failed for [%d] %s%s", $id, $name, PHP_EOL));
    }
}

function priorityFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'torrent');

    $torrents = runnerTorrents($payload);
    if ($torrents === [])
        runnerFail('no torrents selected');

    $parameters = runnerParameters($payload);
    $order = orderType::byID;
    if (($parameters['by_remain'] ?? false) === true)
        $order = orderType::byRemain;
    elseif (($parameters['by_ratio'] ?? false) === true)
        $order = orderType::byRatio;

    $maxHigh = $parameters['max_high'] ?? 1000;
    if (!is_int($maxHigh) || $maxHigh < 0)
        runnerFail('parameter max_high must be a non-negative integer');

    priorityProcess(runnerRpc($payload), $torrents, $order, $maxHigh);
}

function priorityFromCli(): void
{
    runnerMapArgvAliases([
        '--high' => '--max-high',
        '--ratio' => '--by-ratio',
        '--remain' => '--by-remain',
    ]);

    $defaultSite = file_exists('server.txt') ? trim(file_get_contents('server.txt')) : 'amdocs';
    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, $defaultSite)
        ->addParameter('ids', 't|id', 'set torrent ids', ParameterType::Number, ParameterCount::Multiple, Mandatory::Mandatory)
        ->addParameter('max_high', 'h|max-high', 'max high', ParameterType::Number, ParameterCount::Single, default: 1000)
        ->addParameter('by_ratio', 'r|by-ratio', 'sort by ratio', ParameterType::Boolean)
        ->addParameter('by_remain', 'e|by-remain', 'sort by remain', ParameterType::Boolean)
        ->Process();

    $server = GetServer($par->site);
    if ($server === false)
        runnerFail('no server available');

    $rpc = new TransmissionRPC($server, use_netrc: true);
    if (!$rpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    $order = orderType::byID;
    if ($par->by_remain)
        $order = orderType::byRemain;
    elseif ($par->by_ratio)
        $order = orderType::byRatio;

    $maxHigh = (int) $par->max_high;
    if ($maxHigh < 0)
        runnerFail('parameter max_high must be a non-negative integer');

    $result = $rpc->get($par->ids, ['id', 'name']);
    if (!TransmissionRPC::check_result($result, 'torrents'))
        runnerFail('no torrent found');

    $torrents = [];
    foreach ($result->arguments->torrents as $torrent)
    {
        $torrents[] = [
            'torrent_id' => (int) $torrent->id,
            'name' => (string) $torrent->name,
        ];
    }

    priorityProcess($rpc, $torrents, $order, $maxHigh);
}

$payload = runnerTryReadPayload();
if ($payload === null)
    priorityFromCli();
else
    priorityFromPayload($payload);
