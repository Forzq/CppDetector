param([string]$Output = 'artifacts/frames36-study')
$ErrorActionPreference = 'Stop'
function Read-Rows([string]$Name) {
    @(Get-Content -LiteralPath (Join-Path $Output $Name) | ForEach-Object { ConvertFrom-Json $_ })
}
$dataset = Read-Rows 'dataset.jsonl'
# Capture boundaries differ between cameras, as checked on contact sheets.
function Has-Label($r) {
    $id = [int]$r.sample.Substring(2)
    $start = if ($r.cam -lt 5) { 43 } else { 44 }
    $id -lt $start -or $id -gt 62
}
foreach ($r in $dataset) {
    $r | Add-Member -NotePropertyName label_visible -NotePropertyValue (Has-Label $r)
}
$dataset | Export-Csv -LiteralPath (Join-Path $Output 'dataset.csv') -NoTypeInformation -Encoding UTF8
$cameraStats = foreach ($g in ($dataset | Group-Object cam)) {
    $label = @($g.Group | Where-Object label_visible)
    $blank = @($g.Group | Where-Object { -not $_.label_visible })
    [pscustomobject]@{
        cam = [int]$g.Name
        frames = $g.Count
        label_frames = $label.Count
        label_rejects = @($label | Where-Object reject).Count
        label_diff_rejects = @($label | Where-Object diff_reject).Count
        label_seam_rejects = @($label | Where-Object seam_reject).Count
        blank_frames = $blank.Count
        blank_rejects = @($blank | Where-Object reject).Count
        shift_rejects = @($g.Group | Where-Object reason -eq 'shift_exceeded').Count
    }
}
$cameraStats | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'dataset_summary.json') -Encoding UTF8
$sweep = Read-Rows 'sweep.jsonl'
$sweep | Export-Csv -LiteralPath (Join-Path $Output 'sweep.csv') -NoTypeInformation -Encoding UTF8
$variantStats = foreach ($g in ($sweep | Group-Object variant)) {
    $natural = @($g.Group | Where-Object { -not $_.synthetic })
    $label = @($natural | Where-Object { Has-Label $_ })
    $blank = @($natural | Where-Object { -not (Has-Label $_) -and $_.cam -notin 2,7 })
    $challenge = @($g.Group | Where-Object { $_.synthetic -and $_.challenge })
    $nuisance = @($g.Group | Where-Object { $_.synthetic -and -not $_.challenge -and $_.kind -ne 'background' })
    [pscustomobject]@{
        variant = $g.Name
        label_n = $label.Count
        label_rejects = @($label | Where-Object reject).Count
        label_diff_rejects = @($label | Where-Object diff_reject).Count
        blank_n = $blank.Count
        blank_rejects = @($blank | Where-Object reject).Count
        challenge_n = $challenge.Count
        challenge_rejects = @($challenge | Where-Object reject).Count
        challenge_diff_rejects = @($challenge | Where-Object diff_reject).Count
        challenge_zone_hits = @($challenge | Where-Object zone_hits_defect).Count
        nuisance_n = $nuisance.Count
        nuisance_rejects = @($nuisance | Where-Object reject).Count
        inspect_mean_ms = ($g.Group | Measure-Object inspect_ms -Average).Average
    }
}
$variantStats | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'variants_summary.json') -Encoding UTF8
$seam = Read-Rows 'seam.jsonl'
$seam | Export-Csv -LiteralPath (Join-Path $Output 'seam.csv') -NoTypeInformation -Encoding UTF8
$seamStats = foreach ($g in ($seam | Group-Object line,variant)) {
    $real = @($g.Group | Where-Object { $_.kind -notin 'bend','bow','gap','middle_capture' })
    [pscustomobject]@{
        line = $g.Group[0].line
        variant = $g.Group[0].variant
        labelled_n = $real.Count
        labelled_found = @($real | Where-Object found).Count
        labelled_rejects = @($real | Where-Object reject).Count
        bend_n = @($g.Group | Where-Object kind -eq 'bend').Count
        bend_rejects = @($g.Group | Where-Object { $_.kind -eq 'bend' -and $_.reject }).Count
    }
}
$seamStats | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'seam_summary.json') -Encoding UTF8
$cameraStats | Format-Table -AutoSize
$variantStats | Format-Table -AutoSize
$seamStats | Where-Object variant -eq configured | Format-Table -AutoSize
