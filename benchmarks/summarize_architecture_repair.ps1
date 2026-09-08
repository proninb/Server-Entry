param([string]$Results = (Join-Path $PSScriptRoot '..\out\architecture_repair'))
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
function Get-RepairMedian($Values) {
    $ordered = @($Values | Sort-Object)
    if (!$ordered.Count) { throw 'Cannot summarize empty measurements' }
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if ($ordered.Count % 2) { return $ordered[$middle] }
    return ($ordered[$middle - 1] + $ordered[$middle]) / 2
}
$processMedians = foreach ($side in @('baseline', 'candidate')) {
    for ($process = 1; $process -le 4; ++$process) {
        $suffix = if ($process -eq 1) { '' } else { '_' + $process }
        $rows = Import-Csv -LiteralPath (Join-Path $Results ($side + '_raw' + $suffix + '.csv')) |
            Where-Object { $_.shape -notin @('churn', 'type_churn') -and $_.run -ne '0' }
        foreach ($group in ($rows | Group-Object shape,n,phase)) {
            if ($group.Count -ne 3) { throw "Expected three measured iterations: $side $process $($group.Name)" }
            $values = @($group.Group | ForEach-Object { [double]::Parse($_.ms, $culture) })
            if (@($values | Where-Object { $_ -le 0 -or [double]::IsNaN($_) -or [double]::IsInfinity($_) }).Count) {
                throw "Invalid measurement: $side $process $($group.Name)"
            }
            [pscustomobject]@{
                side = $side; process = $process; shape = $group.Group[0].shape
                n = $group.Group[0].n; phase = $group.Group[0].phase
                ms = Get-RepairMedian $values
            }
        }
    }
}
$processMedians | Export-Csv -LiteralPath (Join-Path $Results 'process_medians.csv') -NoTypeInformation
$comparison = foreach ($group in ($processMedians | Group-Object shape,n,phase)) {
    $before = @($group.Group | Where-Object side -eq baseline | ForEach-Object ms)
    $after = @($group.Group | Where-Object side -eq candidate | ForEach-Object ms)
    if ($before.Count -ne 4 -or $after.Count -ne 4) { throw 'Expected four processes per version' }
    $b = Get-RepairMedian $before
    $a = Get-RepairMedian $after
    [pscustomobject]@{
        shape = $group.Group[0].shape; n = $group.Group[0].n; phase = $group.Group[0].phase
        baseline_ms = $b; candidate_ms = $a; speedup = $b / $a
        candidate_min = ($after | Measure-Object -Minimum).Minimum
        candidate_max = ($after | Measure-Object -Maximum).Maximum
    }
}
$comparison | Export-Csv -LiteralPath (Join-Path $Results 'comparison.csv') -NoTypeInformation
$comparison | Format-Table -AutoSize
