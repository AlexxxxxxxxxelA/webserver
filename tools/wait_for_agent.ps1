param(
    [int]$TimeoutSeconds = 120,
    [string]$StatusFile = "",
    [string]$Uri = "http://127.0.0.1:18081/health"
)

$deadline = [DateTime]::UtcNow.AddSeconds([Math]::Max(0, $TimeoutSeconds))

do {
    if ($StatusFile -and (Test-Path -LiteralPath $StatusFile)) {
        exit 2
    }

    try {
        $response = Invoke-RestMethod -UseBasicParsing -Uri $Uri -TimeoutSec 1
        if ($response.status -eq "ok" -and
            $response.service -eq "cpp-webserver-agent") {
            exit 0
        }
    }
    catch {
        # The server may still be compiling or starting. Retry until the deadline.
    }

    if ($TimeoutSeconds -le 0) {
        exit 1
    }
    Start-Sleep -Milliseconds 200
} while ([DateTime]::UtcNow -lt $deadline)

exit 1
