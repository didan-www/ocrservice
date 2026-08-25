[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Executable,
    [Parameter(Mandatory)] [string] $QtMqttBin,
    [Parameter(Mandatory)] [string] $QtBin,
    [Parameter(Mandatory)] [string] $MingwBin,
    [ValidateSet('Login', 'Full')] [string] $Slice = 'Full',
    [string] $SshHost = 'ocrservice-vm',
    [string] $RemoteScript = '/home/share/ocrservice/tests/system/qt_e2e/remote-compose.sh',
    [string] $ServerBaseUrl = 'http://192.168.137.128:8080',
    [string] $ArtifactRoot,
    [switch] $KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'UiAutomation.ps1')

function ConvertFrom-CodePoints {
    param([Parameter(Mandatory)] [int[]] $CodePoints)
    return -join ($CodePoints | ForEach-Object { [char] $_ })
}

$TextHttpOnline = ConvertFrom-CodePoints 0x5728, 0x7ebf
$TextMqttConnected = ConvertFrom-CodePoints 0x5df2, 0x8fde, 0x63a5
$TextMqttReconnecting = ConvertFrom-CodePoints 0x91cd, 0x8fde, 0x4e2d
$TextMqttPermanent = ConvertFrom-CodePoints 0x672a, 0x8fde, 0x63a5, 0xff08, 0x914d, 0x7f6e, 0x6216, 0x6388, 0x6743, 0x9519, 0x8bef, 0xff09
$TextRecognizing = ConvertFrom-CodePoints 0x8bc6, 0x522b, 0x4e2d
$TextCompleted = ConvertFrom-CodePoints 0x5df2, 0x8bc6, 0x522b, 0x5b8c, 0x6210
$TextImageLoading = ConvertFrom-CodePoints 0x56fe, 0x7247, 0x52a0, 0x8f7d, 0x4e2d, 0x002e, 0x002e, 0x002e
$TextImageFailed = ConvertFrom-CodePoints 0x56fe, 0x7247, 0x52a0, 0x8f7d, 0x5931, 0x8d25
$TextExportCsv = (ConvertFrom-CodePoints 0x5bfc, 0x51fa) + ' CSV'
$TextSave = ConvertFrom-CodePoints 0x4fdd, 0x5b58
$TextCsvSaved = 'CSV ' + (ConvertFrom-CodePoints 0x5df2, 0x4fdd, 0x5b58, 0x3002)
$TextWhiteList = ConvertFrom-CodePoints 0x767d, 0x540d, 0x5355
$TextBlackList = ConvertFrom-CodePoints 0x9ed1, 0x540d, 0x5355
$TextCreated = ConvertFrom-CodePoints 0x65b0, 0x589e, 0x5df2, 0x751f, 0x6548, 0x3002
$TextConfirmDelete = ConvertFrom-CodePoints 0x786e, 0x8ba4, 0x5220, 0x9664
$TextYes = ConvertFrom-CodePoints 0x662f
$TextEnglishYes = 'Yes'
$TextDeleted = ConvertFrom-CodePoints 0x5220, 0x9664, 0x5df2, 0x751f, 0x6548, 0x3002
$CsvHeader = @(
    (ConvertFrom-CodePoints 0x8bb0, 0x5f55) + 'ID'
    ConvertFrom-CodePoints 0x8bbe, 0x5907
    ConvertFrom-CodePoints 0x8f66, 0x724c, 0x53f7
    ConvertFrom-CodePoints 0x72b6, 0x6001
    ConvertFrom-CodePoints 0x9519, 0x8bef, 0x7801
    ConvertFrom-CodePoints 0x9519, 0x8bef, 0x4fe1, 0x606f
    ConvertFrom-CodePoints 0x62cd, 0x6444, 0x65f6, 0x95f4
    ConvertFrom-CodePoints 0x5f00, 0x59cb, 0x65f6, 0x95f4
    ConvertFrom-CodePoints 0x5b8c, 0x6210, 0x65f6, 0x95f4
    (ConvertFrom-CodePoints 0x8017, 0x65f6) + '(ms)'
) -join ','

function Assert-Path {
    param([Parameter(Mandatory)] [string] $Path, [Parameter(Mandatory)] [string] $Label)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Label does not exist: $Path"
    }
}

function Invoke-Remote {
    param(
        [Parameter(Mandatory)] [string] $Action,
        [AllowEmptyString()] [string] $StandardInput = ''
    )

    $arguments = @('-T', $SshHost, 'bash', $RemoteScript, $Action, $script:RunId)
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($StandardInput) {
            $output = $StandardInput | & ssh @arguments 2>&1
        } else {
            $output = & ssh @arguments 2>&1
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($exitCode -ne 0) {
        throw "Remote action '$Action' failed: $($output -join [Environment]::NewLine)"
    }
    return ($output -join [Environment]::NewLine).Trim()
}

function Write-Stage {
    param([Parameter(Mandatory)] [string] $Name)
    Write-Output "qt-e2e: stage=$Name"
}

function Login-Client {
    param(
        [Parameter(Mandatory)] [System.Diagnostics.Process] $Process,
        [int] $MainWindowTimeoutSeconds = 30
    )

    $username = Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.usernameEdit'
    $password = Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.passwordEdit'
    $button = Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.loginButton'
    Set-UiaValue -Element $username -Value $script:Username
    Set-UiaValue -Element $password -Value $script:Password
    Invoke-UiaElement -Element $button
    [void] (Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.mainWindow' `
        -TimeoutSeconds $MainWindowTimeoutSeconds)
}

function Invoke-VisibleButtonByPrefix {
    param(
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Within,
        [Parameter(Mandatory)] [string] $Prefix,
        [string[]] $AlternatePrefixes = @(),
        [int] $ExpectedProcessId = 0,
        [int] $TimeoutSeconds = 20
    )

    $withinProcessId = $Within.Current.ProcessId
    if ($ExpectedProcessId -ne 0 -and $withinProcessId -ne $ExpectedProcessId) {
        throw "The target window does not belong to process $ExpectedProcessId."
    }
    $prefixes = @($Prefix) + $AlternatePrefixes
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $elements = $Within.FindAll(
            [System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        foreach ($element in $elements) {
            $current = $element.Current
            if (($ExpectedProcessId -eq 0 -or $current.ProcessId -eq $ExpectedProcessId) -and
                -not $current.IsOffscreen -and
                $current.ControlType -eq [System.Windows.Automation.ControlType]::Button -and
                $current.IsEnabled) {
                foreach ($candidatePrefix in $prefixes) {
                    if ($current.Name.StartsWith($candidatePrefix, [StringComparison]::Ordinal)) {
                        Invoke-UiaElement -Element $element
                        return
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    Write-UiaButtonEvidence -Within $Within -Context 'prefix-not-found' `
        -ExpectedProcessId $ExpectedProcessId
    throw "No visible button starts with '$Prefix'."
}

function ConvertTo-UiaEvidenceText {
    param([AllowNull()] [string] $Value)

    if ($null -eq $Value) {
        return '<null>'
    }
    $sanitized = [Text.RegularExpressions.Regex]::Replace($Value, '[\x00-\x1f\x7f]', '?')
    $sanitized = $sanitized.Replace("'", "''")
    if ($sanitized.Length -gt 80) {
        return $sanitized.Substring(0, 80) + '...'
    }
    return $sanitized
}

function Write-UiaButtonEvidence {
    param(
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Within,
        [Parameter(Mandatory)] [string] $Context,
        [int] $ExpectedProcessId = 0
    )

    try {
        $dialog = $Within.Current
        Write-Output ("qt-e2e: uiaEvidence context={0} dialogProcessId={1} processIdMatch={2} dialogName='{3}'" -f
            $Context,
            $dialog.ProcessId,
            ($ExpectedProcessId -eq 0 -or $dialog.ProcessId -eq $ExpectedProcessId),
            (ConvertTo-UiaEvidenceText $dialog.Name))
        $elements = $Within.FindAll(
            [System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        $legacyPattern = [System.Windows.Automation.AutomationPattern]::LookupById(10018)
        $index = 0
        foreach ($element in $elements) {
            try {
                $current = $element.Current
                if (($ExpectedProcessId -ne 0 -and $current.ProcessId -ne $ExpectedProcessId) -or
                    $current.ControlType -ne [System.Windows.Automation.ControlType]::Button) {
                    continue
                }
                $invoke = Get-UiaPattern -Element $element `
                    -Pattern ([System.Windows.Automation.InvokePattern]::Pattern)
                $legacyState = 'unavailable'
                if ($null -ne $legacyPattern) {
                    $legacy = Get-UiaPattern -Element $element -Pattern $legacyPattern
                    $legacyState = ($null -ne $legacy).ToString()
                }
                $bounds = $current.BoundingRectangle
                Write-Output ("qt-e2e: uiaButton index={0} processId={1} name='{2}' automationId='{3}' controlType={4} invoke={5} legacy={6} offscreen={7} enabled={8} focus={9} bounds={10},{11},{12},{13}" -f
                    $index,
                    $current.ProcessId,
                    (ConvertTo-UiaEvidenceText $current.Name),
                    (ConvertTo-UiaEvidenceText $current.AutomationId),
                    $current.ControlType.ProgrammaticName,
                    ($null -ne $invoke),
                    $legacyState,
                    $current.IsOffscreen,
                    $current.IsEnabled,
                    $current.HasKeyboardFocus,
                    [int] $bounds.X,
                    [int] $bounds.Y,
                    [int] $bounds.Width,
                    [int] $bounds.Height)
                ++$index
            } catch {
                Write-Output ("qt-e2e: uiaButtonUnavailable type={0} message='{1}'" -f
                    $_.Exception.GetType().Name,
                    (ConvertTo-UiaEvidenceText $_.Exception.Message))
                continue
            }
        }
        Write-Output "qt-e2e: uiaButtonCount=$index"
    } catch {
        Write-Output ("qt-e2e: uiaEvidenceUnavailable type={0} message='{1}'" -f
            $_.Exception.GetType().Name,
            (ConvertTo-UiaEvidenceText $_.Exception.Message))
    }
}

function Find-NativeFileNameEdit {
    param(
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Dialog,
        [int] $TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            $candidates = @()
            $elements = $Dialog.FindAll(
                [System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($element in $elements) {
                try {
                    $current = $element.Current
                    if ($current.IsOffscreen -or
                        $current.ControlType -ne [System.Windows.Automation.ControlType]::Edit -or
                        $current.AutomationId.IndexOf('Search', [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                        continue
                    }
                    if ($null -ne (Get-UiaPattern -Element $element `
                                      -Pattern ([System.Windows.Automation.ValuePattern]::Pattern))) {
                        $candidates += $element
                    }
                } catch [System.Windows.Automation.ElementNotAvailableException] {
                    continue
                }
            }
            if ($candidates.Count -gt 0) {
                return $candidates[$candidates.Count - 1]
            }
        } catch [System.Windows.Automation.ElementNotAvailableException] {
            # The native dialog populates its shell controls asynchronously.
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The native save dialog did not expose a file-name editor.'
}

function Wait-ImageLoaded {
    param([Parameter(Mandatory)] [System.Diagnostics.Process] $Process)

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $lastState = 'missing'
    do {
        $image = Find-UiaElement -ProcessId $Process.Id `
            -AutomationIdSuffix '.currentImageLabel' -TimeoutSeconds 1
        $name = $image.Current.Name
        $retryHidden = $true
        try {
            $retry = Find-UiaElement -ProcessId $Process.Id `
                -AutomationIdSuffix '.imageRetryButton' -TimeoutSeconds 1 -IncludeOffscreen
            $retryHidden = $retry.Current.IsOffscreen
        } catch {
            $retryHidden = $true
        }
        if ([string]::IsNullOrEmpty($name) -and $retryHidden) {
            return
        }
        $lastState = if ($name -eq $TextImageLoading) {
            'loading'
        } elseif ($name -eq $TextImageFailed) {
            'failed'
        } else {
            "nameLength=$($name.Length) retryHidden=$retryHidden"
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The realtime image did not finish loading; state=$lastState."
}

function Wait-MqttPermanentWithTrace {
    param(
        [Parameter(Mandatory)] [System.Diagnostics.Process] $Process,
        [Parameter(Mandatory)] [string] $Expected,
        [Parameter(Mandatory)] [string] $HttpOnline,
        [int] $TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $last = $null
    $actual = '<missing>'
    do {
        try {
            $element = Find-UiaElement -ProcessId $Process.Id `
                -AutomationIdSuffix '.mqttStatusIndicator.statusValueLabel' `
                -TimeoutSeconds 1 -IncludeOffscreen
            $actual = $element.Current.Name
            if ($actual -ne $last) {
                Write-Output ("qt-e2e: mqttStatus='{0}'" -f
                    (ConvertTo-UiaEvidenceText $actual))
                $last = $actual
            }
            if ($actual -eq $Expected) {
                $http = Wait-UiaText -ProcessId $Process.Id `
                    -AutomationIdSuffix '.httpStatusIndicator.statusValueLabel' `
                    -Expected $HttpOnline -TimeoutSeconds 5
                Write-Output ("qt-e2e: mqttPermanent=1 httpStatus='{0}'" -f
                    (ConvertTo-UiaEvidenceText $http.Current.Name))
                return
            }
        } catch {
            if ($last -ne '<unavailable>') {
                Write-Output 'qt-e2e: mqttStatus=''<unavailable>'''
                $last = '<unavailable>'
            }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $httpStatus = '<missing>'
    try {
        $http = Find-UiaElement -ProcessId $Process.Id `
            -AutomationIdSuffix '.httpStatusIndicator.statusValueLabel' `
            -TimeoutSeconds 1 -IncludeOffscreen
        $httpStatus = $http.Current.Name
    } catch {
        $httpStatus = '<unavailable>'
    }
    throw ("MQTT did not enter permanent error; actual='{0}' httpStatus='{1}'." -f
        (ConvertTo-UiaEvidenceText $actual),
        (ConvertTo-UiaEvidenceText $httpStatus))
}

function Assert-CsvArtifact {
    param([Parameter(Mandatory)] [string] $Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -le 3 -or $bytes[0] -ne 0xef -or
        $bytes[1] -ne 0xbb -or $bytes[2] -ne 0xbf) {
        throw 'The GUI CSV export is empty or lacks the required UTF-8 BOM.'
    }
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes, 3, $bytes.Length - 3)
    if (-not $text.StartsWith($CsvHeader + "`r`n", [StringComparison]::Ordinal)) {
        throw 'The GUI CSV export does not have the fixed ten-column header.'
    }
    if ($text -match '(?<!\r)\n' -or $text -match '\r(?!\n)' -or
        -not $text.EndsWith("`r`n", [StringComparison]::Ordinal)) {
        throw 'The GUI CSV export does not use CRLF for every record.'
    }
    if ($text.IndexOf('device-task023', [StringComparison]::Ordinal) -lt 0 -or
        $text.IndexOf('QT023A', [StringComparison]::Ordinal) -lt 0) {
        throw 'The GUI CSV export does not contain the current TASK-023 recognition.'
    }
}

function Select-AccessListTab {
    param(
        [Parameter(Mandatory)] [System.Diagnostics.Process] $Process,
        [Parameter(Mandatory)] [string] $Name
    )

    $tabs = Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.accessListTabs'
    $tab = Find-UiaElement -ProcessId $Process.Id -Within $tabs -Name $Name `
        -ControlType ([System.Windows.Automation.ControlType]::TabItem)
    Select-UiaElement -Element $tab
}

function Create-AccessListRecord {
    param(
        [Parameter(Mandatory)] [System.Diagnostics.Process] $Process,
        [Parameter(Mandatory)] [string] $Plate,
        [Parameter(Mandatory)] [string] $Remark,
        [Parameter(Mandatory)] [string] $ExpectedType
    )

    $create = Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.accessListCreateButton'
    Invoke-UiaElement -Element $create
    $dialog = Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.accessListCreateDialog'
    $type = Find-UiaElement -ProcessId $Process.Id -Within $dialog `
        -AutomationIdSuffix '.accessListCreateTypeLabel'
    $plateEdit = Find-UiaElement -ProcessId $Process.Id -Within $dialog `
        -AutomationIdSuffix '.accessListCreatePlateEdit'
    $remarkEdit = Find-UiaElement -ProcessId $Process.Id -Within $dialog `
        -AutomationIdSuffix '.accessListCreateRemarkEdit'
    Set-UiaValue -Element $plateEdit -Value $Plate
    Set-UiaValue -Element $remarkEdit -Value $Remark
    $plateValue = Get-UiaPattern -Element $plateEdit `
        -Pattern ([System.Windows.Automation.ValuePattern]::Pattern)
    $remarkValue = Get-UiaPattern -Element $remarkEdit `
        -Pattern ([System.Windows.Automation.ValuePattern]::Pattern)
    if ($type.Current.Name -ne $ExpectedType -or $null -eq $plateValue -or
        $plateValue.Current.Value -ne $Plate -or $null -eq $remarkValue -or
        $remarkValue.Current.Value -ne $Remark) {
        throw 'The access-list dialog did not retain the expected type and input values.'
    }
    Write-Output 'qt-e2e: accessInputMatched=1 typeMatched=1'
    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $Process.Id `
        -AutomationIdSuffix '.accessListCreateSubmitButton')
}

function Open-HistoryAndSearch {
    param([Parameter(Mandatory)] [System.Diagnostics.Process] $Process)

    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $Process.Id `
        -AutomationIdSuffix '.historyNavButton')
    [void] (Find-UiaElement -ProcessId $Process.Id -AutomationIdSuffix '.historyPage')
    Set-UiaValue -Element (Find-UiaElement -ProcessId $Process.Id `
        -AutomationIdSuffix '.historyDeviceEdit') -Value 'device-task023'
    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $Process.Id `
        -AutomationIdSuffix '.historySearchButton')
}

Assert-Path -Path $Executable -Label 'Qt executable'
Assert-Path -Path $QtMqttBin -Label 'Qt MQTT runtime directory'
Assert-Path -Path $QtBin -Label 'Qt runtime directory'
Assert-Path -Path $MingwBin -Label 'MinGW runtime directory'

$script:Username = [Environment]::GetEnvironmentVariable('QT_E2E_USERNAME')
$script:Password = [Environment]::GetEnvironmentVariable('QT_E2E_PASSWORD')
if ([string]::IsNullOrWhiteSpace($script:Username) -or
    [string]::IsNullOrWhiteSpace($script:Password)) {
    throw 'QT_E2E_USERNAME and QT_E2E_PASSWORD must be set in the runner environment.'
}

$clientId = [Guid]::NewGuid().ToString('D').ToLowerInvariant()
$script:RunId = $clientId.Replace('-', '').Substring(0, 12)
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "ocrservice-qt-e2e-$($script:RunId)"
if ([string]::IsNullOrWhiteSpace($ArtifactRoot)) {
    $ArtifactRoot = Join-Path $temporaryRoot 'artifacts'
}
$runtimeRoot = Join-Path $temporaryRoot 'runtime'
$configRoot = Join-Path $runtimeRoot 'config'
$process = $null
$remoteStarted = $false
$originalPath = $env:PATH
$completed = $false

try {
    $existingClients = @(Get-Process -Name 'plate_client' -ErrorAction SilentlyContinue)
    if ($existingClients.Count -ne 0) {
        throw 'A plate_client process already exists; close it before running isolated UI evidence.'
    }
    New-Item -ItemType Directory -Path $configRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $ArtifactRoot -Force | Out-Null
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $runtimeRoot 'plate_client.exe')
    $config = [ordered]@{
        clientId = $clientId
        loginBaseUrl = $ServerBaseUrl
        allowInsecureTransport = $true
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText(
        (Join-Path $configRoot 'client.json'),
        $config + "`n",
        [Text.UTF8Encoding]::new($false))

    Write-Stage 'compose-start'
    $remoteStarted = $true
    $startResult = Invoke-Remote -Action 'start'
    if ($startResult -notmatch 'stack=healthy') {
        throw "Remote start did not return a healthy marker: $startResult"
    }
    Write-Stage 'qt-start'
    $env:PATH = "$QtMqttBin;$QtBin;$MingwBin;$originalPath"
    $process = Start-Process -FilePath (Join-Path $runtimeRoot 'plate_client.exe') `
        -WorkingDirectory $runtimeRoot -PassThru `
        -RedirectStandardOutput (Join-Path $ArtifactRoot 'qt.stdout.log') `
        -RedirectStandardError (Join-Path $ArtifactRoot 'qt.stderr.log')
    [void] (Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.loginWindow' -TimeoutSeconds 20)

    Write-Stage 'login-and-suback'
    Login-Client -Process $process
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.httpStatusIndicator.statusValueLabel' `
        -Expected $TextHttpOnline -TimeoutSeconds 20)
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.mqttStatusIndicator.statusValueLabel' `
        -Expected $TextMqttConnected -TimeoutSeconds 20)
    $mainWindow = Get-UiaProcessRoot -ProcessId $process.Id
    Save-UiaScreenshot -Element $mainWindow -Path (Join-Path $ArtifactRoot '01-login-connected.png')

    if ($Slice -eq 'Login') {
        Write-Output 'qt e2e login slice passed: login=1 httpOnline=1 mqttConnectSuback=1'
        $completed = $true
        return
    }

    Write-Stage 'processing-final-image'
    $seedResult = Invoke-Remote -Action 'seed-processing'
    if ($seedResult -notmatch 'processing=published') {
        throw "Processing seed did not complete: $seedResult"
    }
    $probeBody = [ordered]@{
        username = $script:Username
        password = $script:Password
        clientId = [Guid]::NewGuid().ToString('D').ToLowerInvariant()
    } | ConvertTo-Json -Compress
    $probeResult = Invoke-Remote -Action 'probe-image' -StandardInput $probeBody
    if ($probeResult -notmatch 'status=200 contentType=image/jpeg' -or
        $probeResult -notmatch 'match=1') {
        throw "The independent image probe failed: $probeResult"
    }
    [void] (Wait-UiaText -ProcessId $process.Id -AutomationIdSuffix '.currentStatusLabel' `
        -Expected $TextRecognizing -TimeoutSeconds 20)
    Wait-ImageLoaded -Process $process
    Save-UiaScreenshot -Element (Get-UiaProcessRoot -ProcessId $process.Id) `
        -Path (Join-Path $ArtifactRoot '02-processing-image.png')
    $finishResult = Invoke-Remote -Action 'finish'
    if ($finishResult -notmatch 'status=SUCCEEDED') {
        throw "Final recognition did not complete: $finishResult"
    }
    [void] (Wait-UiaText -ProcessId $process.Id -AutomationIdSuffix '.currentStatusLabel' `
        -Expected $TextCompleted -TimeoutSeconds 20)
    [void] (Wait-UiaText -ProcessId $process.Id -AutomationIdSuffix '.currentPlateLabel' `
        -Expected 'QT023A' -TimeoutSeconds 20)
    Save-UiaScreenshot -Element (Get-UiaProcessRoot -ProcessId $process.Id) `
        -Path (Join-Path $ArtifactRoot '03-final-recognition.png')

    Write-Stage 'history-and-csv'
    Open-HistoryAndSearch -Process $process
    $historyTable = Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.historyTable'
    [void] (Wait-UiaDescendantName -ProcessId $process.Id -Within $historyTable `
        -Expected 'device-task023' -TimeoutSeconds 20)
    $csvPath = Join-Path $ArtifactRoot 'recognitions.csv'
    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $process.Id `
        -AutomationIdSuffix '.historyExportButton')
    $csvDialog = Find-UiaElement -ProcessId $process.Id -Name $TextExportCsv `
        -ControlType ([System.Windows.Automation.ControlType]::Window)
    $fileNameEdit = Find-NativeFileNameEdit -Dialog $csvDialog
    Set-UiaValue -Element $fileNameEdit -Value $csvPath
    Invoke-VisibleButtonByPrefix -Within $csvDialog -Prefix $TextSave `
        -ExpectedProcessId $process.Id
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.historyExportStatusLabel' -Expected $TextCsvSaved -TimeoutSeconds 20)
    $csvDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $csvPath) -and [DateTime]::UtcNow -lt $csvDeadline) {
        Start-Sleep -Milliseconds 100
    }
    Assert-CsvArtifact -Path $csvPath
    Save-UiaScreenshot -Element (Get-UiaProcessRoot -ProcessId $process.Id) `
        -Path (Join-Path $ArtifactRoot '04-history.png')

    Write-Stage 'access-list'
    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $process.Id `
        -AutomationIdSuffix '.accessListNavButton')
    [void] (Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.accessListPage')
    Create-AccessListRecord -Process $process -Plate 'QT023A' -Remark 'TASK-023 UI E2E' `
        -ExpectedType $TextWhiteList
    [void] (Wait-UiaText -ProcessId $process.Id -AutomationIdSuffix '.accessListStatusLabel' `
        -Expected $TextCreated -TimeoutSeconds 20)
    Select-AccessListTab -Process $process -Name $TextBlackList
    Create-AccessListRecord -Process $process -Plate 'QT023A' -Remark 'conflict probe' `
        -ExpectedType $TextBlackList
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.accessListCreateErrorLabel' -Expected $TextWhiteList `
        -Contains -TimeoutSeconds 20)
    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $process.Id `
        -AutomationIdSuffix '.accessListCreateCancelButton')
    Select-AccessListTab -Process $process -Name $TextWhiteList
    $accessTable = Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.accessListTable'
    $plateCell = Wait-UiaDescendantName -ProcessId $process.Id -Within $accessTable `
        -Expected 'QT023A' -TimeoutSeconds 20
    Select-UiaElement -Element $plateCell
    Invoke-UiaElement -Element (Find-UiaElement -ProcessId $process.Id `
        -AutomationIdSuffix '.accessListDeleteButton')
    $processRoot = Get-UiaProcessRoot -ProcessId $process.Id
    $deleteDialog = Find-UiaElement -ProcessId $process.Id -Within $processRoot `
        -Name $TextConfirmDelete -ControlType ([System.Windows.Automation.ControlType]::Window)
    Invoke-VisibleButtonByPrefix -Within $deleteDialog -Prefix $TextYes `
        -AlternatePrefixes @($TextEnglishYes) -ExpectedProcessId $process.Id
    [void] (Wait-UiaText -ProcessId $process.Id -AutomationIdSuffix '.accessListStatusLabel' `
        -Expected $TextDeleted -TimeoutSeconds 20)
    Save-UiaScreenshot -Element (Get-UiaProcessRoot -ProcessId $process.Id) `
        -Path (Join-Path $ArtifactRoot '05-access-list.png')

    Write-Stage 'mqtt-temporary-recovery'
    [void] (Invoke-Remote -Action 'mqtt-stop')
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.mqttStatusIndicator.statusValueLabel' `
        -Expected $TextMqttReconnecting -TimeoutSeconds 20)
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.httpStatusIndicator.statusValueLabel' `
        -Expected $TextHttpOnline -TimeoutSeconds 5)
    [void] (Invoke-Remote -Action 'mqtt-start')
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.mqttStatusIndicator.statusValueLabel' `
        -Expected $TextMqttConnected -TimeoutSeconds 30)
    $connectedEvidence = Invoke-Remote -Action 'evidence'
    if ($connectedEvidence -notmatch 'mqttConnect=([1-9][0-9]*)') {
        throw "The recovered broker has no successful client evidence: $connectedEvidence"
    }

    Write-Stage 'mqtt-permanent-http-independent'
    [void] (Invoke-Remote -Action 'mqtt-bad')
    Wait-MqttPermanentWithTrace -Process $process -Expected $TextMqttPermanent `
        -HttpOnline $TextHttpOnline -TimeoutSeconds 30
    Open-HistoryAndSearch -Process $process
    $historyTable = Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.historyTable'
    [void] (Wait-UiaDescendantName -ProcessId $process.Id -Within $historyTable `
        -Expected 'device-task023' -TimeoutSeconds 20)
    [void] (Wait-UiaText -ProcessId $process.Id `
        -AutomationIdSuffix '.httpStatusIndicator.statusValueLabel' `
        -Expected $TextHttpOnline -TimeoutSeconds 5)
    Save-UiaScreenshot -Element (Get-UiaProcessRoot -ProcessId $process.Id) `
        -Path (Join-Path $ArtifactRoot '06-mqtt-permanent-http-online.png')
    $permanentEvidence = Invoke-Remote -Action 'evidence'
    if ($permanentEvidence -notmatch 'apiPost=([1-9][0-9]*)' -or
        $permanentEvidence -notmatch 'image=([1-9][0-9]*)' -or
        $permanentEvidence -notmatch 'apiGet=([1-9][0-9]*)' -or
        $permanentEvidence -notmatch 'mqttConnect=0(?:\s|$)' -or
        $permanentEvidence -notmatch 'mqttAttempts=([1-9][0-9]*)' -or
        $permanentEvidence -notmatch 'mqttAuthRejected=([1-9][0-9]*)') {
        throw "Sanitized permanent-error evidence is incomplete: $permanentEvidence"
    }
    $serviceEvidence = "connected=[$connectedEvidence] permanent=[$permanentEvidence]"
    [void] (Invoke-Remote -Action 'mqtt-good')

    Write-Stage 'http-401-return-to-login'
    $logoutButton = Find-UiaElement -ProcessId $process.Id `
        -AutomationIdSuffix '.logoutButton'
    $supersedeBody = [ordered]@{
        username = $script:Username
        password = $script:Password
        clientId = $clientId
    } | ConvertTo-Json -Compress
    [void] (Invoke-Remote -Action 'supersede' -StandardInput $supersedeBody)
    Invoke-UiaElement -Element $logoutButton
    [void] (Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.loginWindow' `
        -TimeoutSeconds 20)
    $preExpiryEvidence = Invoke-Remote -Action 'evidence'
    $serviceEvidence += " preExpiry=[$preExpiryEvidence]"

    Write-Stage 'token-expiry-return-to-login'
    $successCountBeforeExpiry =
        [Text.RegularExpressions.Regex]::Matches($preExpiryEvidence, 'POST:200:OK').Count
    [void] (Invoke-Remote -Action 'short-ttl')
    Login-Client -Process $process -MainWindowTimeoutSeconds 8
    [void] (Find-UiaElement -ProcessId $process.Id -AutomationIdSuffix '.loginWindow' `
        -TimeoutSeconds 20)
    $expiryEvidence = Invoke-Remote -Action 'evidence-stop'
    $successCountAfterExpiry =
        [Text.RegularExpressions.Regex]::Matches($expiryEvidence, 'POST:200:OK').Count
    if ($expiryEvidence -notmatch 'POST:401:AUTH_TOKEN_INVALID' -or
        $successCountAfterExpiry -le $successCountBeforeExpiry) {
        throw "The final 401 or short-TTL evidence is incomplete: $expiryEvidence"
    }
    $serviceEvidence += " expiry=[$expiryEvidence]"

    $csvHash = (Get-FileHash -LiteralPath $csvPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "qt e2e passed: login=1 heartbeat=1 mqttSuback=1 processing=1 final=1 image=1 history=1 csv=1 accessCreate=1 accessConflict=1 accessDelete=1 mqttTemporary=1 mqttPermanentHttp=1 http401=1 expiry=1 csvSha256=$csvHash"
    Write-Output "qt-e2e: serverEvidence=$serviceEvidence"
    $completed = $true
} catch {
    $failure = $_
    if ($remoteStarted) {
        try {
            Write-Output "qt-e2e: failureEvidence=$(Invoke-Remote -Action 'evidence-stop')"
        } catch {
            Write-Output 'qt-e2e: failureEvidence=unavailable'
        }
    }
    if ($null -ne $process -and -not $process.HasExited) {
        try {
            Save-UiaScreenshot -Element (Get-UiaProcessRoot -ProcessId $process.Id) `
                -Path (Join-Path $ArtifactRoot 'failure.png')
        } catch {
            Write-Output 'qt-e2e: failureScreenshot=unavailable'
        }
    }
    throw $failure
} finally {
    $env:PATH = $originalPath
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(5000) | Out-Null
    }
    if ($remoteStarted) {
        Write-Stage 'cleanup'
        $cleanupResult = Invoke-Remote -Action 'cleanup'
        Write-Output $cleanupResult
        if ($cleanupResult -cnotmatch
            '^cleanup containers=0 volumes=0 networks=0 protected=true hostMosquitto=active$') {
            throw "Remote cleanup invariants failed: $cleanupResult"
        }
    }
    if (-not $KeepArtifacts -and (Test-Path -LiteralPath $temporaryRoot)) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    } elseif ($KeepArtifacts) {
        Write-Output "qt-e2e: artifacts=$ArtifactRoot"
    }
    if (-not $completed -and $Slice -eq 'Login') {
        Write-Output 'qt-e2e: login slice did not complete'
    }
}
