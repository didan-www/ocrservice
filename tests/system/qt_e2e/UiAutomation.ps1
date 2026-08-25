Set-StrictMode -Version Latest

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

function Get-UiaProcessRoot {
    param(
        [Parameter(Mandatory)] [int] $ProcessId,
        [int] $TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
            $ProcessId)
        $elements = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Children,
            $condition)
        $element = $elements | Where-Object { -not $_.Current.IsOffscreen } |
            Sort-Object { $_.Current.BoundingRectangle.Width * $_.Current.BoundingRectangle.Height } `
                -Descending | Select-Object -First 1
        if ($null -ne $element) {
            return $element
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "UI Automation did not find a top-level window for process $ProcessId."
}

function Find-UiaElement {
    param(
        [Parameter(Mandatory)] [int] $ProcessId,
        [string] $AutomationIdSuffix,
        [string] $Name,
        [System.Windows.Automation.ControlType] $ControlType,
        [int] $TimeoutSeconds = 20,
        [System.Windows.Automation.AutomationElement] $Within,
        [switch] $IncludeOffscreen
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($null -ne $Within) {
            $root = $Within
            $scope = [System.Windows.Automation.TreeScope]::Subtree
            $condition = [System.Windows.Automation.Condition]::TrueCondition
        } else {
            $root = [System.Windows.Automation.AutomationElement]::RootElement
            $scope = [System.Windows.Automation.TreeScope]::Descendants
            $condition = [System.Windows.Automation.PropertyCondition]::new(
                [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
                $ProcessId)
        }
        try {
            $elements = $root.FindAll($scope, $condition)
            foreach ($element in $elements) {
                try {
                    $current = $element.Current
                    if (-not $IncludeOffscreen -and $current.IsOffscreen) {
                        continue
                    }
                    if ($AutomationIdSuffix -and
                        -not $current.AutomationId.EndsWith($AutomationIdSuffix,
                                                           [StringComparison]::Ordinal)) {
                        continue
                    }
                    if ($Name -and $current.Name -ne $Name) {
                        continue
                    }
                    if ($null -ne $ControlType -and $current.ControlType -ne $ControlType) {
                        continue
                    }
                    return $element
                } catch [System.Windows.Automation.ElementNotAvailableException] {
                    continue
                }
            }
        } catch [System.Windows.Automation.ElementNotAvailableException] {
            # Qt rebuilds accessibility nodes while stacked pages and dialogs change.
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $description = @($AutomationIdSuffix, $Name) | Where-Object { $_ }
    throw "UI Automation element was not found: $($description -join ', ')."
}

function Get-UiaPattern {
    param(
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Element,
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationPattern] $Pattern
    )

    $value = $null
    if (-not $Element.TryGetCurrentPattern($Pattern, [ref] $value)) {
        return $null
    }
    return $value
}

function Set-UiaValue {
    param(
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Element,
        [Parameter(Mandatory)] [AllowEmptyString()] [string] $Value
    )

    $pattern = Get-UiaPattern -Element $Element -Pattern ([System.Windows.Automation.ValuePattern]::Pattern)
    if ($null -eq $pattern) {
        throw "Element '$($Element.Current.AutomationId)' does not support ValuePattern."
    }
    $pattern.SetValue($Value)
}

function Invoke-UiaElement {
    param([Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Element)

    $invoke = Get-UiaPattern -Element $Element -Pattern ([System.Windows.Automation.InvokePattern]::Pattern)
    if ($null -ne $invoke) {
        $invoke.Invoke()
        return
    }
    $legacy = Get-UiaPattern -Element $Element -Pattern ([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
    if ($null -ne $legacy) {
        $legacy.DoDefaultAction()
        return
    }
    throw "Element '$($Element.Current.AutomationId)' has no invokable pattern."
}

function Select-UiaElement {
    param([Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Element)

    $selection = Get-UiaPattern -Element $Element -Pattern ([System.Windows.Automation.SelectionItemPattern]::Pattern)
    if ($null -ne $selection) {
        $selection.Select()
        return
    }
    Invoke-UiaElement -Element $Element
}

function Wait-UiaText {
    param(
        [Parameter(Mandatory)] [int] $ProcessId,
        [Parameter(Mandatory)] [string] $AutomationIdSuffix,
        [Parameter(Mandatory)] [string] $Expected,
        [switch] $Contains,
        [int] $TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            $element = Find-UiaElement -ProcessId $ProcessId `
                -AutomationIdSuffix $AutomationIdSuffix -TimeoutSeconds 1
            $actual = $element.Current.Name
            if (($Contains -and $actual.IndexOf($Expected, [StringComparison]::Ordinal) -ge 0) -or
                (-not $Contains -and $actual -eq $Expected)) {
                return $element
            }
        } catch {
            # The window may be transitioning between login and authenticated views.
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Timed out waiting for '$AutomationIdSuffix' to show '$Expected'."
}

function Wait-UiaDescendantName {
    param(
        [Parameter(Mandatory)] [int] $ProcessId,
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Within,
        [Parameter(Mandatory)] [string] $Expected,
        [switch] $Contains,
        [int] $TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            $elements = $Within.FindAll(
                [System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($element in $elements) {
                try {
                    $name = $element.Current.Name
                    if (($Contains -and $name.IndexOf($Expected, [StringComparison]::Ordinal) -ge 0) -or
                        (-not $Contains -and $name -eq $Expected)) {
                        return $element
                    }
                } catch [System.Windows.Automation.ElementNotAvailableException] {
                    continue
                }
            }
        } catch [System.Windows.Automation.ElementNotAvailableException] {
            # Re-evaluate the current subtree until the bounded deadline.
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for descendant text '$Expected'."
}

function Save-UiaScreenshot {
    param(
        [Parameter(Mandatory)] [System.Windows.Automation.AutomationElement] $Element,
        [Parameter(Mandatory)] [string] $Path
    )

    $bounds = $Element.Current.BoundingRectangle
    if ($bounds.Width -lt 1 -or $bounds.Height -lt 1) {
        throw 'Cannot capture a UI element with empty bounds.'
    }
    $bitmap = [System.Drawing.Bitmap]::new([int] $bounds.Width, [int] $bounds.Height)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen([int] $bounds.X, [int] $bounds.Y, 0, 0, $bitmap.Size)
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
}
