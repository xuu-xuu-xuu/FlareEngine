import { onUnmounted, ref } from 'vue'

export function useGuideFeedback() {
  const squashed = ref(false)
  let started = 0, releaseTimer, audio, pressToneEnd = 0

  // 短促的下滑音和较轻的上滑音。首次触摸时浏览器可能到抬手才允许发声。
  function tone(release) {
    try {
      const Audio = window.AudioContext || window.webkitAudioContext
      if (!Audio) return
      audio ??= new Audio()
      if (audio.state !== 'running') void audio.resume().catch(() => {})
      const start = release ? Math.max(pressToneEnd, audio.currentTime) : audio.currentTime
      const end = start + (release ? .07 : .085)
      const oscillator = audio.createOscillator()
      const gain = audio.createGain()
      oscillator.frequency.setValueAtTime(release ? 480 : 640, start)
      oscillator.frequency.exponentialRampToValueAtTime(release ? 700 : 230, end)
      gain.gain.setValueAtTime(.0001, start)
      gain.gain.exponentialRampToValueAtTime(release ? .07 : .12, start + .006)
      gain.gain.exponentialRampToValueAtTime(.0001, end)
      oscillator.connect(gain).connect(audio.destination)
      oscillator.onended = () => { oscillator.disconnect(); gain.disconnect() }
      oscillator.start(start)
      oscillator.stop(end)
      if (!release) pressToneEnd = end
    } catch { /* 浏览器禁用音频时，角色仍可交互。 */ }
  }
  function press() {
    clearTimeout(releaseTimer)
    started = performance.now()
    squashed.value = true
    tone(false)
  }
  function release(cancelled = false) {
    if (!cancelled) tone(true)
    // 快速轻点也要给浏览器留下足够时间画出压缩帧。
    releaseTimer = setTimeout(() => { squashed.value = false },
      cancelled ? 0 : Math.max(0, 145 - (performance.now() - started)))
  }
  onUnmounted(() => {
    clearTimeout(releaseTimer)
    if (audio) void audio.close().catch(() => {})
  })
  return { squashed, press, release }
}
