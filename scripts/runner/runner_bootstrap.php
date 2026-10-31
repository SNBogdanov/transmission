<?php
declare(strict_types=1);

$runnerIncludePath = get_include_path();
foreach ([dirname(__DIR__), __DIR__] as $runnerDir)
{
    if (is_dir($runnerDir))
        $runnerIncludePath = $runnerDir . PATH_SEPARATOR . $runnerIncludePath;
}
set_include_path($runnerIncludePath);

require_once "functions.php";
require_once "TransmissionRPC.class.php";

use Netrc\Netrc;

function runnerFail(string $message, int $exitCode = 1): never
{
    fwrite(STDERR, $message . PHP_EOL);
    exit($exitCode);
}

function runnerTryReadPayload(): ?array
{
    if (function_exists('stream_isatty') && @stream_isatty(STDIN))
        return null;

    $raw = stream_get_contents(STDIN);
    if ($raw === false || trim($raw) === '')
        return null;

    try
    {
        $payload = json_decode($raw, true, 512, JSON_THROW_ON_ERROR);
    }
    catch (JsonException $e)
    {
        runnerFail('invalid stdin payload: ' . $e->getMessage());
    }

    if (!is_array($payload) || array_is_list($payload))
        runnerFail('stdin payload must be a JSON object');
    if (($payload['schema'] ?? null) !== 'transgui.script/v1')
        runnerFail('unsupported payload schema');

    return $payload;
}

function runnerReadPayload(): array
{
    $payload = runnerTryReadPayload();
    if ($payload === null)
        runnerFail('stdin payload is empty');

    return $payload;
}

function runnerRequireContext(array $payload, string $context): void
{
    if (($payload['context'] ?? null) !== $context)
        runnerFail('unsupported payload context');
}

function runnerParameters(array $payload): array
{
    $parameters = $payload['parameters'] ?? [];
    if (!is_array($parameters))
        runnerFail('payload parameters must be an object');

    return $parameters;
}

function runnerTorrents(array $payload): array
{
    $torrents = $payload['torrents'] ?? [];
    if (!is_array($torrents))
        runnerFail('payload torrents must be an array');

    $result = [];
    foreach ($torrents as $torrent)
    {
        if (!is_array($torrent))
            runnerFail('payload torrent must be an object');
        $result[] = $torrent;
    }

    return $result;
}

function runnerTorrentId(array $torrent): int
{
    $id = $torrent['torrent_id'] ?? null;
    if (!is_int($id))
        runnerFail('payload torrent_id is missing');

    return $id;
}

function runnerTorrentHash(array $torrent): string
{
    $hash = $torrent['hash'] ?? null;
    if (!is_string($hash) || preg_match('/^[0-9a-f]{40}$/', $hash) !== 1)
        runnerFail('payload torrent hash is invalid');

    return $hash;
}

function runnerDefaultNetrcPath(): ?string
{
    $home = getenv('HOME');
    if (!is_string($home) || $home === '')
    {
        $home = getenv('USERPROFILE');
        if (!is_string($home) || $home === '')
            return null;
    }

    $name = DIRECTORY_SEPARATOR === '\\' ? '_netrc' : '.netrc';
    return rtrim($home, "/\\") . DIRECTORY_SEPARATOR . $name;
}

function runnerRpcCandidates(string $url): array
{
    $path = parse_url($url, PHP_URL_PATH);
    if (!is_string($path) || $path === '')
        return [$url];

    $base = rtrim($url, '/');
    return array_values(array_unique([$url, $base . '/rpc']));
}

function runnerRpcEndpointWorks(string $url, ?string $username, ?string $password): bool
{
    $headers = "Content-Type: application/json\r\n";
    if ($username !== null && $password !== null)
        $headers .= "Authorization: Basic " . base64_encode($username . ':' . $password) . "\r\n";

    $context = stream_context_create([
        'http' => [
            'method' => 'POST',
            'header' => $headers,
            'content' => '{"method":"session-get"}',
            'timeout' => 5,
            'ignore_errors' => true,
            'follow_location' => 0,
        ],
    ]);

    @file_get_contents($url, false, $context);
    if (!isset($http_response_header[0]))
        return false;
    if (preg_match('/\s(\d{3})\s/', $http_response_header[0], $matches) !== 1)
        return false;

    return in_array((int) $matches[1], [200, 409], true);
}

function runnerRpcFromEndpoint(string $url, ?string $netrcFile = null, ?string $netrcMachine = null): TransmissionRPC
{
    if ($url === '')
        runnerFail('rpc url is missing');

    if ($netrcFile === null || $netrcFile === '')
    {
        $environmentNetrc = getenv('TR_SCRIPT_RPC_NETRC');
        if (is_string($environmentNetrc) && $environmentNetrc !== '')
        {
            $netrcFile = $environmentNetrc;
        }
        else
        {
            $defaultNetrc = runnerDefaultNetrcPath();
            $netrcFile = is_string($defaultNetrc) && is_file($defaultNetrc) && is_readable($defaultNetrc) ? $defaultNetrc : null;
        }
    }

    $username = null;
    $password = null;
    if ($netrcFile !== null)
    {
        try
        {
            $entries = Netrc::parse($netrcFile);
        }
        catch (Throwable $e)
        {
            runnerFail('cannot read netrc: ' . $e->getMessage());
        }

        if ($netrcMachine === null || $netrcMachine === '')
        {
            $host = parse_url($url, PHP_URL_HOST);
            if (!is_string($host) || $host === '')
                runnerFail('rpc url has no host');
            $netrcMachine = $host;
        }

        $machine = Netrc::normalizeHost($netrcMachine);
        if (!isset($entries[$machine]))
            runnerFail('netrc has no machine entry for ' . $machine);

        $username = $entries[$machine]['login'] ?? null;
        $password = $entries[$machine]['password'] ?? null;
    }

    $selectedUrl = $url;
    foreach (runnerRpcCandidates($url) as $candidate)
    {
        if (!runnerRpcEndpointWorks($candidate, $username, $password))
            continue;

        $selectedUrl = $candidate;
        break;
    }

    $client = new TransmissionRPC($selectedUrl, $username, $password);
    if (!$client->connected)
        runnerFail('cannot connect to Transmission RPC');

    return $client;
}

function runnerRpc(array $payload): TransmissionRPC
{
    $rpc = $payload['rpc'] ?? null;
    if (!is_array($rpc))
        runnerFail('payload rpc is missing');

    $url = $rpc['url'] ?? null;
    if (!is_string($url) || $url === '')
        runnerFail('payload rpc.url is missing');

    $netrcFile = $rpc['netrc'] ?? null;
    if (!is_string($netrcFile))
        $netrcFile = null;

    return runnerRpcFromEndpoint($url, $netrcFile);
}

function runnerExternalHost(array $payload, string $id): array
{
    $hosts = $payload['external_hosts'] ?? null;
    if (!is_array($hosts) || !isset($hosts[$id]) || !is_array($hosts[$id]))
        runnerFail('external host is not available: ' . $id);

    $host = $hosts[$id];
    if (!is_string($host['rpc_url'] ?? null) || $host['rpc_url'] === '')
        runnerFail('external host has no rpc_url: ' . $id);

    return $host;
}

function runnerMapArgvAliases(array $aliases): void
{
    if (!isset($GLOBALS['argv']) || !is_array($GLOBALS['argv']))
        return;

    foreach ($GLOBALS['argv'] as $index => $argument)
    {
        if (isset($aliases[$argument]))
            $GLOBALS['argv'][$index] = $aliases[$argument];
    }
}

function runnerSelectedTorrentIds(array $payload): array
{
    $ids = [];
    foreach (runnerTorrents($payload) as $torrent)
        $ids[] = runnerTorrentId($torrent);

    return $ids;
}
