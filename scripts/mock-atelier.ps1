<#
.SYNOPSIS
    Serves a small mock of the InterSystems Atelier v1 API.

.DESCRIPTION
    The mock is intended for manual tests of the mounted file system. It
    implements the endpoints that ISFS uses for reading and writing documents:

        GET    /api/atelier/v1/<namespace>
        GET    /api/atelier/v1/<namespace>/docnames/<category>
        GET    /api/atelier/v1/<namespace>/doc/<dotted-name>
        PUT    /api/atelier/v1/<namespace>/doc/<dotted-name>
        DELETE /api/atelier/v1/<namespace>/doc/<dotted-name>

    A document is either text (enc=false, an array of lines that the client
    joins with LF) or binary (enc=true, base64 chunks). A save replaces the
    document as a whole; the timestamp of the document is its hash, so the
    mock also demonstrates the conditional write (If-None-Match / 409).

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

function Get-CategoryForName([string] $Name)
{
    switch ([IO.Path]::GetExtension($Name).ToLowerInvariant())
    {
        '.cls' { return 'CLS' }
        '.csp' { return 'CSP' }
        '.mac' { return 'RTN' }
        '.int' { return 'RTN' }
        '.rtn' { return 'RTN' }
        default { return 'OTH' }
    }
}

function New-Timestamp
{
    return (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')
}

function Send-Response(
    [System.IO.Stream] $Stream,
    [string] $Method,
    [int] $StatusCode,
    [string] $Body,
    [string] $ETag = $null)
{
    $statusText = switch ($StatusCode)
    {
        200 { 'OK' }
        201 { 'Created' }
        400 { 'Bad Request' }
        404 { 'Not Found' }
        409 { 'Conflict' }
        423 { 'Locked' }
        500 { 'Internal Server Error' }
        501 { 'Not Implemented' }
        default { 'Error' }
    }

    $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($Body)
    $head = "HTTP/1.1 $StatusCode $statusText`r`n" +
        "Content-Type: application/json; charset=utf-8`r`n" +
        "Content-Length: $($bodyBytes.Length)`r`n"
    if ($null -ne $ETag -and '' -ne $ETag)
    {
        $head += "ETag: `"$ETag`"`r`n"
    }
    $head += "Connection: close`r`n`r`n"
    $headBytes = [System.Text.Encoding]::ASCII.GetBytes($head)

    $Stream.Write($headBytes, 0, $headBytes.Length)
    if ('HEAD' -ne $Method)
    {
        $Stream.Write($bodyBytes, 0, $bodyBytes.Length)
    }
    $Stream.Flush()
}

# Reads one HTTP request as raw bytes: the body may be UTF-8 and its length is
# given by Content-Length, so a StreamReader cannot be used for the whole
# request.
function Read-Request([System.IO.Stream] $Stream)
{
    $head = New-Object System.Collections.Generic.List[byte]
    $one = New-Object byte[] 1

    while ($true)
    {
        $read = $Stream.Read($one, 0, 1)
        if ($read -le 0)
        {
            return $null
        }

        $head.Add($one[0])

        $count = $head.Count
        if ($count -ge 4 -and
            $head[$count - 4] -eq 13 -and $head[$count - 3] -eq 10 -and
            $head[$count - 2] -eq 13 -and $head[$count - 1] -eq 10)
        {
            break
        }
    }

    $headText = [System.Text.Encoding]::ASCII.GetString($head.ToArray())
    $lines = $headText -split "`r`n"
    $parts = $lines[0] -split ' '
    if ($parts.Count -lt 2)
    {
        return $null
    }

    $headers = @{}
    for ($i = 1; $i -lt $lines.Count; $i++)
    {
        if ($lines[$i] -match '^([^:]+):\s*(.*)$')
        {
            $headers[$Matches[1].ToLowerInvariant()] = $Matches[2]
        }
    }

    $length = 0
    if ($headers.ContainsKey('content-length'))
    {
        [void][int]::TryParse($headers['content-length'], [ref]$length)
    }

    $body = New-Object byte[] $length
    $offset = 0
    while ($offset -lt $length)
    {
        $read = $Stream.Read($body, $offset, $length - $offset)
        if ($read -le 0)
        {
            break
        }
        $offset += $read
    }

    return [pscustomobject]@{
        Method  = $parts[0]
        Target  = $parts[1]
        Path    = ($parts[1] -split '\?', 2)[0]
        Query   = ($parts[1] -split '\?', 2)[1]
        Headers = $headers
        Body    = [System.Text.Encoding]::UTF8.GetString($body, 0, $offset)
    }
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
            $request = Read-Request $stream

            if ($null -ne $request)
            {
                $method = $request.Method
                $path = $request.Path

                $statusCode = 200
                $body = $null
                $etag = $null

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
                    $docName = [System.Uri]::UnescapeDataString($Matches[1])
                    $document = $documents |
                        Where-Object { $_.Name -eq $docName } |
                        Select-Object -First 1

                    if ('GET' -eq $method -or 'HEAD' -eq $method)
                    {
                        if ($null -eq $document)
                        {
                            $statusCode = 404
                            $body = New-ErrorJson "document $docName does not exist"
                        }
                        else
                        {
                            $body = New-DocumentJson $document
                            $etag = $document.Ts
                        }
                    }
                    elseif ('PUT' -eq $method)
                    {
                        $payload = $null
                        try
                        {
                            $payload = $request.Body | ConvertFrom-Json
                        }
                        catch
                        {
                            $payload = $null
                        }

                        if ($null -eq $payload -or $null -eq $payload.content)
                        {
                            $statusCode = 400
                            $body = New-ErrorJson 'the request body is not a document'
                        }
                        else
                        {
                            # the timestamp is the hash of the document, as on a real
                            # server; If-None-Match makes the write conditional
                            $ifNoneMatch = $request.Headers['if-none-match']
                            if ($null -ne $ifNoneMatch)
                            {
                                $ifNoneMatch = $ifNoneMatch.Trim().Trim('"')
                            }
                            $ignoreConflict = $request.Query -match 'ignoreConflict=1'

                            if ($null -ne $document -and -not $ignoreConflict -and
                                $null -ne $ifNoneMatch -and '' -ne $ifNoneMatch -and
                                $ifNoneMatch -ne $document.Ts)
                            {
                                $statusCode = 409
                                $body = New-ErrorJson "document $docName has another version"
                            }
                            else
                            {
                                $newContent = @($payload.content | ForEach-Object { [string]$_ })
                                $newEnc = [bool]$payload.enc

                                if ($null -eq $document)
                                {
                                    $document = [pscustomobject]@{
                                        Name      = $docName
                                        Cat       = Get-CategoryForName $docName
                                        Ts        = New-Timestamp
                                        Enc       = $newEnc
                                        Generated = $false
                                        Content   = $newContent
                                    }
                                    $documents += $document
                                    $statusCode = 201
                                }
                                else
                                {
                                    $document.Enc = $newEnc
                                    $document.Content = $newContent
                                    $document.Ts = New-Timestamp
                                    $statusCode = 200
                                }

                                $body = New-DocumentJson $document
                                $etag = $document.Ts
                            }
                        }
                    }
                    elseif ('DELETE' -eq $method)
                    {
                        if ($null -eq $document)
                        {
                            $statusCode = 404
                            $body = New-ErrorJson "document $docName does not exist"
                        }
                        else
                        {
                            $documents = @($documents | Where-Object { $_.Name -ne $docName })
                            $body = New-DocumentJson $document
                        }
                    }
                    else
                    {
                        $statusCode = 501
                        $body = New-ErrorJson "the mock does not implement $method"
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
                Send-Response $stream $method $statusCode $body $etag
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
