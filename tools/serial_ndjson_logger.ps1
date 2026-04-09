param(
  [Parameter(Mandatory = $true)][string]$Port,
  [int]$Baud = 115200,
  [string]$Out = "debug-8351e9.log",
  [string]$Session = "8351e9"
)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.NewLine = "`n"
$sp.ReadTimeout = 500
$sp.Open()

function Write-NDJsonLine([hashtable]$obj) {
  $json = ($obj | ConvertTo-Json -Compress -Depth 6)
  Add-Content -Path $Out -Value $json -Encoding UTF8
}

$runId = "host-$([int][double]::Parse((Get-Date -UFormat %s)))"
Write-NDJsonLine @{
  sessionId    = $Session
  runId        = $runId
  hypothesisId = "HOST"
  location     = "serial_ndjson_logger.ps1"
  message      = "serial logger started"
  data         = @{ port = $Port; baud = $Baud }
  timestamp    = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
}

try {
  while ($true) {
    try {
      $line = $sp.ReadLine().Trim()
      if ([string]::IsNullOrWhiteSpace($line)) { continue }

      if ($line.StartsWith("{") -and $line.EndsWith("}")) {
        try {
          $obj = $line | ConvertFrom-Json -ErrorAction Stop
          if ($null -ne $obj.sessionId -and $obj.sessionId -ne $Session) { continue }
          Add-Content -Path $Out -Value $line -Encoding UTF8
          continue
        } catch {}
      }

      Write-NDJsonLine @{
        sessionId    = $Session
        runId        = $runId
        hypothesisId = "HOST"
        location     = "serial_ndjson_logger.ps1"
        message      = "serial line"
        data         = @{ line = $line }
        timestamp    = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
      }
    } catch {}
  }
} finally {
  $sp.Close()
}

