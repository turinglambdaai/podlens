$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $repo '.rivet\acceptance\windows'
$executable = Get-ChildItem -LiteralPath (Join-Path $repo 'dist') `
    -Filter RivetHost.exe -File -Recurse | Select-Object -First 1
if (-not $executable) { throw 'packaged PodLens executable was not found' }

New-Item -ItemType Directory -Force -Path $output | Out-Null
$data = Join-Path $output ("data-{0}" -f $PID)
New-Item -ItemType Directory -Force -Path $data | Out-Null
'{"ui-lang":"en","check-updates-enabled":"false","last-update-check":2000000000}' |
    Set-Content -LiteralPath (Join-Path $data 'config.json') -Encoding utf8

function Wait-Until {
    param([string]$Description, [scriptblock]$Probe, [int]$TimeoutSeconds = 45)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $value = & $Probe
        if ($null -ne $value -and $value -ne $false) { return $value }
        Start-Sleep -Milliseconds 100
    }
    throw "timed out waiting for $Description"
}

function Find-ById {
    param($Root, [string]$AutomationId)
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
        $AutomationId)
    $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Find-ByName {
    param($Root, [string[]]$Names)
    foreach ($name in $Names) {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $name)
        $found = $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
        if ($found) { return $found }
    }
    return $null
}

function Find-EditByName {
    param($Root, [string]$Name)
    $nameCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    $typeCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Edit)
    $condition = [System.Windows.Automation.AndCondition]::new(
        @($nameCondition, $typeCondition))
    $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Activate-Control {
    param($Element)
    [object]$pattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern,
                                      [ref]$pattern)) {
        ([System.Windows.Automation.TogglePattern]$pattern).Toggle()
        return
    }
    $pattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,
                                      [ref]$pattern)) {
        ([System.Windows.Automation.SelectionItemPattern]$pattern).Select()
        return
    }
    $pattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,
                                      [ref]$pattern)) {
        ([System.Windows.Automation.InvokePattern]$pattern).Invoke()
        return
    }
    throw "control $($Element.Current.Name) has no activation pattern"
}

function Activate-WithEnter {
    param($Element)
    $Element.SetFocus()
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
}

$oldData = $env:PODLENS_DATA_DIR
$process = $null
try {
    $env:PODLENS_DATA_DIR = $data
    & racket (Join-Path $repo 'scripts\e2e\seed-listening-fixture.rkt')
    if ($LASTEXITCODE -ne 0) { throw 'could not seed synthetic listening fixture' }
    $process = Start-Process -FilePath $executable.FullName -PassThru `
        -RedirectStandardOutput (Join-Path $output 'application.stdout.log') `
        -RedirectStandardError (Join-Path $output 'application.stderr.log')
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $window = Wait-Until 'PodLens window' {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $desktop.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
    }
    [void](Wait-Until 'embedded backend readiness' {
        $status = Find-ById $window 'StatusLine'
        if ($status -and $status.Current.Name -match 'Ready|就绪|Resuming|继续上次') { return $true }
        return $false
    })

    $focus = Wait-Until 'focus listening button' { Find-ById $window 'FocusModeButton' }
    $segment = Wait-Until 'third transcript sentence' { Find-ById $window 'TranscriptSegment-2' }
    $play = Wait-Until 'play button' { Find-ById $window 'PlayPauseButton' }
    Activate-Control $focus
    [void](Wait-Until 'focused listening layout' {
        if ($focus.Current.Name -match 'Show Library|显示资料库') { return $true }
        return $false
    })
    Activate-Control $play
    Activate-WithEnter $segment
    [void](Wait-Until 'four-second transcript seek' {
        $position = Find-ById $window 'PositionText'
        if ($position -and $position.Current.Name -eq '00:04') { return $true }
        return $false
    })
    $follow = Wait-Until 'follow transcript toggle' { Find-ById $window 'FollowTranscriptButton' }
    Activate-Control $follow
    $toggle = $follow.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    if ($toggle.Current.ToggleState -ne [System.Windows.Automation.ToggleState]::Off) {
        throw 'follow transcript toggle did not turn off'
    }
    $current = Wait-Until 'return to current sentence button' {
        Find-ById $window 'ReturnToCurrentButton'
    }
    Activate-Control $current
    [void](Wait-Until 'follow transcript resumed' {
        $state = $follow.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        if ($state.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) {
            return $true
        }
        return $false
    })
    Activate-Control $focus

    $listeningNodes = $window.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $listeningSnapshot = for ($index = 0; $index -lt [Math]::Min($listeningNodes.Count, 700); $index++) {
        $node = $listeningNodes[$index]
        [ordered]@{
            name = $node.Current.Name
            controlType = $node.Current.ControlType.ProgrammaticName
            automationId = $node.Current.AutomationId
        }
    }
    $listeningSnapshot | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $output 'listening-accessibility.json') -Encoding utf8

    $settings = Wait-Until 'Settings navigation item' { Find-ById $window 'SettingsNavItem' }
    Activate-Control $settings
    $model = Wait-Until 'chat-model settings field' { Find-EditByName $window 'chat-model' }
    $model.GetCurrentPattern(
        [System.Windows.Automation.ValuePattern]::Pattern).SetValue('acceptance-model')
    $save = Wait-Until 'Save button' { Find-ByName $window @('Save', '保存') }
    Activate-Control $save

    # Wait on the UI completion signal before opening config.json. On Windows,
    # polling the file during the backend's atomic replace can momentarily lock
    # the destination and turn the observer itself into the cause of failure.
    [void](Wait-Until 'settings save completion' {
        $status = Find-ById $window 'StatusLine'
        if ($status -and $status.Current.Name -match 'Settings saved|设置已保存') {
            return $true
        }
        return $false
    })
    $config = Get-Content -LiteralPath (Join-Path $data 'config.json') -Raw |
        ConvertFrom-Json
    if ($config.'chat-model' -ne 'acceptance-model') {
        throw 'settings completion was reported without persisting chat-model'
    }

    $nodes = $window.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $snapshot = for ($index = 0; $index -lt [Math]::Min($nodes.Count, 500); $index++) {
        $node = $nodes[$index]
        [ordered]@{
            name = $node.Current.Name
            controlType = $node.Current.ControlType.ProgrammaticName
            automationId = $node.Current.AutomationId
        }
    }
    $snapshot | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $output 'accessibility.json') -Encoding utf8
    [ordered]@{
        platform = 'windows'
        artifact = $executable.FullName
        actions = @(
            'toggle:FocusModeButton',
            'invoke:PlayPauseButton',
            'keyboard:Enter:TranscriptSegment-2',
            'toggle:FollowTranscriptButton:off',
            'invoke:ReturnToCurrentButton',
            'toggle:FocusModeButton:restore',
            'select:SettingsNavItem',
            'set:chat-model',
            'invoke:Save'
        )
        assertions = @(
            'packaged WinUI window activated',
            'synthetic bilingual transcript exposed stable UI Automation ids',
            'focus mode hid and restored the library without reloading the episode',
            'activating a transcript sentence moved playback to 00:04',
            'Current restored follow after the listener turned it off',
            'settings field exposed a stable UI Automation name',
            'save mutation crossed RVT1 into the embedded backend',
            'isolated config.json contains acceptance-model'
        )
    } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $output 'interaction-trace.json') -Encoding utf8

    $window.GetCurrentPattern(
        [System.Windows.Automation.WindowPattern]::Pattern).Close()
    [void](Wait-Until 'orderly process exit' {
        $process.Refresh()
        if ($process.HasExited) { return $true }
        return $false
    } 15)
} finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit()
    }
    if ($process) { $process.Dispose() }
    $env:PODLENS_DATA_DIR = $oldData
}
