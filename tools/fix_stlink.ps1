$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like '*VID_0483*PID_3748*' }
if ($dev) {
    Write-Host "Found: $($dev.FriendlyName) Status: $($dev.Status)"
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    $dev2 = Get-PnpDevice | Where-Object { $_.InstanceId -like '*VID_0483*PID_3748*' }
    Write-Host "After: $($dev2.FriendlyName) Status: $($dev2.Status)"
} else {
    Write-Host "ST-Link not found in device list"
}
