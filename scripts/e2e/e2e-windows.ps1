$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

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

$oldData = $env:PODLENS_DATA_DIR
$process = $null
try {
    $env:PODLENS_DATA_DIR = $data
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
        if ($status -and $status.Current.Name -match 'Ready|就绪') { return $true }
        return $false
    })

    $settings = Wait-Until 'Settings navigation item' { Find-ById $window 'SettingsNavItem' }
    Activate-Control $settings
    $model = Wait-Until 'chat-model settings field' { Find-EditByName $window 'chat-model' }
    $model.GetCurrentPattern(
        [System.Windows.Automation.ValuePattern]::Pattern).SetValue('acceptance-model')
    $save = Wait-Until 'Save button' { Find-ByName $window @('Save', '保存') }
    Activate-Control $save

    $config = Wait-Until 'settings persisted by the embedded backend' {
        try {
            $value = Get-Content -LiteralPath (Join-Path $data 'config.json') -Raw |
                ConvertFrom-Json
            if ($value.'chat-model' -eq 'acceptance-model') { return $value }
        } catch {}
        return $null
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
        actions = @('select:SettingsNavItem', 'set:chat-model', 'invoke:Save')
        assertions = @(
            'packaged WinUI window activated',
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
