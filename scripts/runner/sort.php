#!/usr/bin/env php
<?php
declare(strict_types=1);

require_once __DIR__ . '/runner_bootstrap.php';

function sortTorrents(TransmissionRPC $rpc, bool $reverse, bool $remain, bool $comment): void
{
    $result = $rpc->get([], ['name', 'id', 'comment', 'leftUntilDone']);
    if (!TransmissionRPC::check_result($result, 'torrents'))
        runnerFail('cannot get torrents');

    if ($remain)
    {
        usort($result->arguments->torrents, function ($x, $y) use ($reverse)
        {
            $xRemain = (int) ($x->leftUntilDone ?? 0);
            $yRemain = (int) ($y->leftUntilDone ?? 0);
            return $reverse ? $yRemain - $xRemain : $xRemain - $yRemain;
        });
    }
    elseif ($comment)
    {
        usort($result->arguments->torrents, function ($x, $y) use ($reverse)
        {
            return $reverse ? strcmp($y->comment, $x->comment) : strcmp($x->comment, $y->comment);
        });
    }
    else
    {
        usort($result->arguments->torrents, function ($x, $y) use ($reverse)
        {
            return $reverse ? strcmp($y->name, $x->name) : strcmp($x->name, $y->name);
        });
    }

    $ids = [];
    $i = 0;
    foreach ($result->arguments->torrents as $torrent)
    {
        printf("%4d[%4d] = %s" . PHP_EOL, $i, $torrent->id, $torrent->name);
        $ids[] = $torrent->id;
        $i++;
    }

    $rpc->queue_top($ids);
}

function sortFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'torrent');
    $parameters = runnerParameters($payload);
    sortTorrents(
        runnerRpc($payload),
        ($parameters['reverse'] ?? false) === true,
        ($parameters['remain'] ?? false) === true,
        ($parameters['comment'] ?? false) === true
    );
}

function sortFromCli(): void
{
    $site = 'localhost';
    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, $site)
        ->addParameter('reverse', 'r|reverse', 'reverse order', ParameterType::Boolean)
        ->addParameter('remain', 'e|remain', 'remain order', ParameterType::Boolean)
        ->addParameter('comment', 'm|comment', 'comment order', ParameterType::Boolean)
        ->Process();

    $server = GetServer($par->site);
    if ($server === false)
        runnerFail('no server available');

    try
    {
        $rpc = new TransmissionRPC($server, use_netrc: true);
    }
    catch (Exception $e)
    {
        runnerFail('cannot connect to Transmission RPC: ' . $e->getMessage());
    }

    if (!$rpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    sortTorrents($rpc, $par->reverse, $par->remain, $par->comment);
}

$payload = runnerTryReadPayload();
if ($payload === null)
    sortFromCli();
else
    sortFromPayload($payload);
