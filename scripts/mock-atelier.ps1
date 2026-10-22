<#
.SYNOPSIS
    Serves a small mock of the InterSystems Atelier v1 API.

.DESCRIPTION
    The mock is intended for manual tests of the mounted file system. It
    implements only the endpoints that ISFS reads:

        GET /api/atelier/v1/<namespace>
        GET /api/atelier/v1/<namespace>/docnames/<category>
        GET /api/atelier/v1/<namespace>/doc/<dotted-name>

    A document is either text (enc=false, an array of lines that the client
    joins with LF) or binary (enc=true, base64 chunks). The mock does not
    implement writes (PUT) yet; it answers 501 to them.

    The server is single-threaded and closes the connection after each
    response, which is enough for the WinHTTP client of ISFS.

.PARAMETER Port
    TCP port to listen on; the listener is bound to loopback only.
    Default: 18080.

.PARAMETER LogFile
    File for the request log. Default: isfs-mock-atelier.log in the temp
    directory.

.PARAMETER Namespace
    Namespace name returned by the namespace endpoint and used in the "db"
    fields of the listing. Default: USER.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\mock-atelier.ps1 -Port 18080

    Then mount the volume:

    isfs.exe -p 127.0.0.1:18080 -ns USER -user tester -pass secret -m X:
#>
[CmdletBinding()]
param(
    [int] $Port = 18080,
    [string] $LogFile = (Join-Path ([IO.Path]::GetTempPath()) 'isfs-mock-atelier.log'),
    [string] $Namespace = 'USER'
)

$ErrorActionPreference = 'Stop'

# The documents exposed by the mock. Enc=false means that Content is the
# array of lines returned as is; Enc=true means that Content contains base64
# chunks that the client concatenates and decodes.
$documents = @(
    [pscustomobject]@{
        Name      = 'User.Person.cls'
        Cat       = 'CLS'
        Ts        = '2016-01-04 14:00:04.000'
        Enc       = $false
        Generated = $false
        Content   = @(
            'Class User.Person Extends %Persistent',
            '{',
            '}',
            ''
        )
    }
    [pscustomobject]@{
        Name      = 'User.Admin.cls'
        Cat       = 'CLS'
        Ts        = '2016-02-05 09:11:12.500'
        Enc       = $true
        Generated = $false
        Content   = @(
            'AAH/',
            'QUI='
        )
    }
    [pscustomobject]@{
        Name      = 'User.Admin.Settings.cls'
        Cat       = 'CLS'
        Ts        = '2016-03-06 10:12:13.250'
        Enc       = $false
        Generated = $false
        Content   = @(
            'Class User.Admin.Settings Extends %Persistent',
            '{',
            '}',
            ''
        )
    }
    [pscustomobject]@{
        Name      = 'index.csp'
        Cat       = 'CSP'
        Ts        = '2016-04-07 11:13:14.750'
        Enc       = $false
        Generated = $false
        Content   = @(
            '<html>',
            '<body>index</body>',
            '</html>',
            ''
        )
    }
    [pscustomobject]@{
        Name      = '%Activate.Enum.cls'
        Cat       = 'CLS'
        Ts        = '2016-05-08 12:14:15.125'
        Enc       = $false
        Generated = $false
        Content   = @(
            'Class %Activate.Enum',
            '{',
            '}',
            ''
        )
    }
    [pscustomobject]@{
        Name      = 'Generated.cls'
        Cat       = 'CLS'
        Ts        = '2016-06-09 13:14:16.875'
        Enc       = $false
        Generated = $true
        Content   = @()
    }
    [pscustomobject]@{
        Name      = 'data.bin'
        Cat       = 'OTH'
        Ts        = '2016-07-10 14:15:16.000'
        Enc       = $true
        Generated = $false
        Content   = @(
            'AAH/QUI='
        )
    }
    [pscustomobject]@{
        Name      = 'Hello.mac'
        Cat       = 'RTN'
        Ts        = '2016-08-11 15:16:17.000'
        Enc       = $false
        Generated = $false
        Content   = @(
            'Hello',
            ' write "Hello"',
            ' quit',
            ''
        )
    }
)

function Write-Log([string] $Message)
{
    "$(Get-Date -Format 'HH:mm:ss.fff') $Message" |
        Out-File -FilePath $LogFile -Append -Encoding utf8
}

function New-ListingJson([string] $Category)
{
    $content = @(
        $documents |
            Where-Object { $_.Cat -eq $Category } |
            ForEach-Object {
                [ordered]@{
                    name = $_.Name
                    cat  = $_.Cat
                    ts   = $_.Ts
                    db   = $Namespace
                    gen  = $_.Generated
                    upd  = $false
                }
            }
    )

    @{
        status  = @{ errors = @(); summary = '' }
        console = @()
        result  = @{ content = $content }
    } | ConvertTo-Json -Depth 10
}

function New-DocumentJson($Document)
{
    @{
        status  = @{ errors = @(); summary = '' }
        console = @()
        result  = [ordered]@{
            name    = $Document.Name
            db      = $Namespace
            ts      = $Document.Ts
            cat     = $Document.Cat
            status  = ''
            enc     = [bool]$Document.Enc
            content = @($Document.Content)
        }
    } | ConvertTo-Json -Depth 10
}

function New-NamespaceJson
{
    @{
        status  = @{ errors = @(); summary = '' }
        console = @()
        result  = @{
            content = @{
                name  = $Namespace
                alias = $Namespace
                type  = 1
            }
        }
    } | ConvertTo-Json -Depth 10
}

function New-ErrorJson([string] $Message)
{
    @{
        status  = @{ errors = @(@{ error = $Message }); summary = $Message }
        console = @()
        result  = @{ status = $Message }
    } | ConvertTo-Json -Depth 10
}

function Send-Response(
    [System.IO.Stream] $Stream,
    [string] $Method,
    [int] $StatusCode,
    [string] $Body)
{
    $statusText = switch ($StatusCode)
    {
        200 { 'OK' }
        404 { 'Not Found' }
        501 { 'Not Implemented' }
        default { 'Error' }
    }

    $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($Body)
    $head = "HTTP/1.1 $StatusCode $statusText`r`n" +
        "Content-Type: application/json; charset=utf-8`r`n" +
        "Content-Length: $($bodyBytes.Length)`r`n" +
        "Connection: close`r`n`r`n"
    $headBytes = [System.Text.Encoding]::ASCII.GetBytes($head)

    $Stream.Write($headBytes, 0, $headBytes.Length)
    if ('HEAD' -ne $Method)
    {
        $Stream.Write($bodyBytes, 0, $bodyBytes.Length)
    }
    $Stream.Flush()
}

"" | Out-File -FilePath $LogFile -Encoding utf8
Write-Log "mock Atelier server listening on http://127.0.0.1:$Port (namespace $Namespace)"

$listener = New-Object System.Net.Sockets.TcpListener(
    [System.Net.IPAddress]::Loopback, $Port)
$listener.Start()

try
{
    while ($true)
    {
        $client = $listener.AcceptTcpClient()
        try
        {
            $stream = $client.GetStream()
            $reader = New-Object System.IO.StreamReader(
                $stream, [System.Text.Encoding]::ASCII)
            $requestLine = $reader.ReadLine()

            if (-not [string]::IsNullOrWhiteSpace($requestLine))
            {
                # the headers are not used, but the blank line must be consumed
                while ($true)
                {
                    $line = $reader.ReadLine()
                    if ($null -eq $line -or '' -eq $line)
                    {
                        break
                    }
                }

                $parts = $requestLine -split ' '
                $method = $parts[0]
                $target = $parts[1]
                $path = ($target -split '\?', 2)[0]

                $statusCode = 200
                $body = $null

                if ($path -match '/docnames/([^/]+)$')
                {
                    $category = $Matches[1]
                    if (@($documents | Where-Object { $_.Cat -eq $category }).Count -gt 0)
                    {
                        $body = New-ListingJson $category
                    }
                    else
                    {
                        $statusCode = 404
                        $body = New-ErrorJson "unknown category $category"
                    }
                }
                elseif ($path -match '/doc/(.+)$')
                {
                    $docName = $Matches[1]
                    $document = $documents |
                        Where-Object { $_.Name -eq $docName } |
                        Select-Object -First 1

                    if ($null -eq $document)
                    {
                        $statusCode = 404
                        $body = New-ErrorJson "document $docName does not exist"
                    }
                    elseif ('GET' -ne $method -and 'HEAD' -ne $method)
                    {
                        $statusCode = 501
                        $body = New-ErrorJson 'the mock does not implement writes yet'
                    }
                    else
                    {
                        $body = New-DocumentJson $document
                    }
                }
                elseif ($path -match '/api/atelier/v1/[^/]+$')
                {
                    $body = New-NamespaceJson
                }
                else
                {
                    $statusCode = 404
                    $body = New-ErrorJson "unknown path $path"
                }

                Write-Log "$method $path -> $statusCode"
                Send-Response $stream $method $statusCode $body
            }
        }
        catch
        {
            Write-Log "error: $($_.Exception.Message)"
        }
        finally
        {
            $client.Close()
        }
    }
}
finally
{
    $listener.Stop()
}
