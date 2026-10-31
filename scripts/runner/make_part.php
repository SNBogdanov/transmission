#!/usr/bin/env php
<?php
declare(strict_types=1);

require_once __DIR__ . '/runner_bootstrap.php';

function makePart(TransmissionRPC $rpc, array $ids, bool $full, bool $allocate, bool $wanted): void
{
    $fields = ['id', 'name', 'totalSize'];
    $res1 = $rpc->get($ids, $fields);
    if (!TransmissionRPC::check_result($res1, 'torrents'))
        return;

    for ($j = 0; $j < count($res1->arguments->torrents); $j++)
    {
        if ($res1->arguments->torrents[$j]->totalSize == 0)
            continue;
        printf("[%4d]=%s", $res1->arguments->torrents[$j]->id, $res1->arguments->torrents[$j]->name);

        $files = ['files', 'wanted', 'id', 'name', 'downloadDir'];
        $res = $rpc->get($res1->arguments->torrents[$j]->id, $files);
        if (!TransmissionRPC::check_result($res, 'torrents'))
            continue;

        $verify = false;
        $torrent = $res->arguments->torrents[0];
        $end = ($wanted || $full) ? count($torrent->files) : 1;
        for ($i = 0; $i < $end; $i++)
        {
            $dir = $torrent->downloadDir . "/";
            $destinationName = $torrent->downloadDir . "/" . $torrent->files[$i]->name;
            if ($rpc->incomplete_dir_enabled)
            {
                $name = $rpc->incomplete_dir . "/" . $torrent->files[$i]->name;
                $dir = $rpc->incomplete_dir . "/";
            }
            else
            {
                $name = $destinationName;
            }

            if (file_exists($destinationName) || file_exists($name))
                continue;

            $partName = '';
            if (file_exists($destinationName . ".part"))
                $partName = $destinationName . ".part";
            elseif (file_exists($name . ".part"))
                $partName = $name . ".part";

            if (!empty($partName))
            {
                if (PHP_OS == "WINNT")
                {
                    $handle = fopen($partName, 'a');
                    if ($handle !== false)
                    {
                        if ($allocate)
                        {
                            $free = disk_free_space(pathinfo($partName, PATHINFO_DIRNAME));
                            if (($free - 5 * 1024 * 1024 * 1024) > $torrent->files[$i]->length)
                                ftruncate($handle, $torrent->files[$i]->length);
                            $verify = true;
                        }
                        fclose($handle);
                    }
                }
                continue;
            }

            $fileWanted = (array_key_exists($i, $torrent->wanted)) ? $torrent->wanted[$i] : 0;
            if (($fileWanted == 1) || $full)
            {
                $dir1 = pathinfo($name, PATHINFO_DIRNAME);
                if (!file_exists($dir1))
                {
                    mk_dir($dir1, 0777, true);
                    if (substr($name, 0, 1) == "/")
                    {
                        $parts = explode('/', str_replace($dir, '', $dir1));
                        foreach ($parts as $part)
                        {
                            $dir = $dir . '/' . $part;
                            MakeReadable($dir, true);
                        }
                    }
                }

                $handle = fopen($name . ".part", 'a');
                if ($handle !== false)
                {
                    if ($allocate)
                    {
                        $free = disk_free_space($dir1);
                        if (($free - 5 * 1024 * 1024 * 1024) > $torrent->files[$i]->length)
                            ftruncate($handle, $torrent->files[$i]->length);
                    }
                    $verify = true;
                    fclose($handle);
                }

                if (substr($name, 0, 1) == "/")
                    MakeReadable($name . ".part");
            }
        }

        if ($verify)
        {
            $rpc->verify($torrent->id);
            printf(" (Verify)" . PHP_EOL, $res1->arguments->torrents[0]->id, $res1->arguments->torrents[0]->name);
            $rpc->start($torrent->id);
        }
        else
        {
            printf(PHP_EOL);
        }
    }
}

function makePartFromPayload(array $payload): void
{
    runnerRequireContext($payload, 'torrent');

    $ids = runnerSelectedTorrentIds($payload);
    if ($ids === [])
        runnerFail('no torrents selected');

    $parameters = runnerParameters($payload);
    makePart(
        runnerRpc($payload),
        $ids,
        ($parameters['full'] ?? false) === true,
        ($parameters['allocate'] ?? false) === true,
        ($parameters['wanted'] ?? false) === true
    );
}

function makePartFromCli(): void
{
    $defaultSite = file_exists('server.txt') ? trim(file_get_contents('server.txt')) : 'amdocs';
    $par = new Parameters();
    $par->addParameter('site', 's|server', 'server', ParameterType::String, ParameterCount::Single, Mandatory::Mandatory, $defaultSite)
        ->addParameter('ids', 't|id', 'set torrent ids', ParameterType::Number, ParameterCount::Multiple, Mandatory::Mandatory)
        ->addParameter('full', 'f|full', 'create full torrent structure', ParameterType::Boolean)
        ->addParameter('allocate', 'a|allocate', 'allocate file size', ParameterType::Boolean)
        ->addParameter('wanted', 'w|wanted', 'create only wanted files', ParameterType::Boolean)
        ->Process();

    $server = GetServer($par->site);
    if ($server === false)
        runnerFail('no server available');

    $rpc = new TransmissionRPC($server, use_netrc: true);
    if (!$rpc->connected)
        runnerFail('cannot connect to Transmission RPC');

    makePart($rpc, $par->ids, $par->full, $par->allocate, $par->wanted);
}

$payload = runnerTryReadPayload();
if ($payload === null)
    makePartFromCli();
else
    makePartFromPayload($payload);
