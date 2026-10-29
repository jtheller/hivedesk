param(
    [ValidateRange(1,3600)][int]$Seconds=30,
    [string]$DataDirectory=(Join-Path $env:LOCALAPPDATA 'HiveDesk')
)
$ErrorActionPreference='Stop'
$latest=Join-Path $DataDirectory 'diagnostics.json'
$deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
$last=''
$staleShown=$false
function Number($value){if($null -eq $value){return '-'};return ([double]$value).ToString('0.0',[Globalization.CultureInfo]::InvariantCulture)}
while([DateTime]::UtcNow -lt $deadline){
    try {
        $sample=Get-Content -LiteralPath $latest -Raw -Encoding UTF8 | ConvertFrom-Json
        $age=([DateTime]::UtcNow-([DateTime]$sample.utc).ToUniversalTime()).TotalSeconds
        if($sample.utc -ne $last){
            $last=$sample.utc;$staleShown=$false
            $prefix=([DateTime]$sample.utc).ToLocalTime().ToString('HH:mm:ss')
            if($sample.status -ne 'streaming'){Write-Output "$prefix $($sample.status)"}
            foreach($role in @('view','host')){
                $stream=$sample.$role;if($null -eq $stream){continue}
                $h=$stream.health;$s=$stream.source;$v=$stream.viewer
                $limits=@();if($h.encode_limit){$limits+='encode'};if($h.decode_limit){$limits+='receiver'};if($h.send_limit){$limits+='send'}
                $reason=if($limits.Count){$limits -join '+'}else{'none'}
                Write-Output ("{0} {1} {2} [{3}x{4}] connected={5} target/cap={6}/{7} RX/Present={8}/{9} source={10}fps encode/color/nvenc={11}/{12}/{13}ms decode/render/present={14}/{15}/{16}ms Mbps={17} queue={18}ms sendwait={19}ms/s limit={20} source-age={21}ms sample-age={22}ms" -f
                    $prefix,$role,$stream.name,$stream.width,$stream.height,$stream.connected,(Number $h.target_fps),(Number $h.cap_fps),
                    (Number $v.rx_fps),(Number $v.present_fps),(Number $s.fps),(Number $s.encode_ms),(Number $s.color_ms),(Number $s.nvenc_ms),
                    (Number $v.decode_ms),(Number $v.render_ms),(Number $v.present_ms),(Number $v.mbps),(Number $v.queue_ms),
                    (Number $s.send_wait_ms_per_s),$reason,(Number $stream.source_age_ms),(Number $s.encode_sample_age_ms))
                if($s.rate_details){
                    $rate=$s.rate
                    $mode=if($rate.automatic_cbr){'AutoCBR'}else{'CQP baseline'}
                    $rateReasons=@('stable','quality','network','quality floor','probe')
                    $rateReason=if($rate.reason -lt $rateReasons.Count){$rateReasons[$rate.reason]}else{'unknown'}
                    Write-Output ("  {0} target/actual={1}/{2}Mbps QP={3} reason={4} receiver={5}ms queue={6}ms recovery={7} source-drop={8} gap/age/overflow={9}/{10}/{11}" -f
                        $mode,(Number $rate.target_mbps),(Number $rate.actual_mbps),(Number $rate.average_qp),$rateReason,
                        (Number $rate.receiver_service_ms),(Number $rate.receiver_queue_ms),$rate.receiver_recovering,$rate.source_discarded,
                        $rate.sequence_gaps,$rate.age_expired,$rate.queue_overflows)
                }
                if($null -ne $v.nvdec_map_ms){
                    Write-Output ("  Viewer map/convert/draw={0}/{1}/{2}ms Present-gap p95/max={3}/{4}ms (includes idle; not scanout)" -f
                        (Number $v.nvdec_map_ms),(Number $v.convert_ms),(Number $v.draw_ms),(Number $v.present_gap_p95_ms),(Number $v.present_gap_max_ms))
                }
                if($null -ne $stream.video_send){
                    $send=$stream.video_send
                    Write-Output ("  Video send p95/max={0}/{1}ms encrypt={2}ms queue={3}ms ready-gap={4}ms samples={5} age={6}ms failed/expired={7}/{8}" -f
                        (Number $send.send_p95_ms),(Number $send.send_max_ms),(Number $send.encrypt_p95_ms),(Number $send.queue_p95_ms),
                        (Number $send.ready_gap_p95_ms),$send.samples,(Number $send.sample_age_ms),$send.failed_packets,$send.queue_expired)
                }
                if($null -ne $stream.input){
                    $inputStats=$stream.input
                    Write-Output ("  Input dedicated={0} ready={1} ACK-roundtrip p50/p99={2}/{3}ms dispatch-p99={4}ms host-queue/inject-p95={5}/{6}ms queued/inflight={7}/{8} samples={9} age={10}ms" -f
                        $inputStats.dedicated,$inputStats.ready,(Number $inputStats.ack_roundtrip_p50_ms),(Number $inputStats.ack_roundtrip_p99_ms),
                        (Number $inputStats.capture_to_send_p99_ms),(Number $inputStats.host_queue_p95_ms),(Number $inputStats.inject_p95_ms),
                        $inputStats.queued,$inputStats.inflight,$inputStats.samples,(Number $inputStats.sample_age_ms))
                }
            }
        }elseif($age -gt 5 -and $sample.status -eq 'streaming' -and -not $staleShown){Write-Output 'STALE: no new diagnostics for more than 5 seconds';$staleShown=$true}
    }catch{if($last -ne 'unavailable'){Write-Output 'Waiting for local diagnostics (recorded automatically by HiveDesk 0.13.1 or later).';$last='unavailable'}}
    Start-Sleep -Milliseconds 500
}
