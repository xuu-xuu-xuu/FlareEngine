import { computed, onMounted, onUnmounted, ref } from 'vue'

const clamp = (n, min, max) => Math.max(min, Math.min(n, max))
const margin = 12

export function useGuidePosition(button, storageKey) {
  const bounds = ref({ left: 0, top: 0, width: 0, height: 0, bw: 0, bh: 0 })
  const fraction = ref([1, 1])
  const dragging = ref(false)
  let pointer, suppressClick = false

  function measure() {
    const vv = window.visualViewport
    bounds.value = {
      left: vv?.offsetLeft ?? 0, top: vv?.offsetTop ?? 0,
      width: vv?.width ?? document.documentElement.clientWidth, height: vv?.height ?? window.innerHeight,
      bw: button.value.offsetWidth, bh: button.value.offsetHeight
    }
  }
  const geometry = computed(() => {
    const { left, top, width, height, bw, bh } = bounds.value
    // 保留顶栏；软键盘打开时改用可见视口。
    const inset = height > 400 ? (width < 960 ? 116 : 76) : margin
    const dx = Math.max(0, width - bw - margin * 2), dy = Math.max(0, height - bh - inset - margin)
    const x = margin + fraction.value[0] * dx, y = inset + fraction.value[1] * dy
    const widthPanel = Math.min(322, width - margin * 2)
    const panelX = clamp(x + bw / 2 - widthPanel / 2, margin, width - widthPanel - margin)
    const above = y - margin >= height - y - bh - margin
    const gap = 10, room = above ? y - margin - gap : height - y - bh - margin - gap
    return { x: left + x, y: top + y, dx, dy, above, panel: {
      width: widthPanel + 'px', left: panelX - x + 'px',
      [above ? 'bottom' : 'top']: bh + gap + 'px',
      '--panel-height': Math.min(340, Math.max(0, room)) + 'px',
      '--tip': x + bw / 2 - panelX + 'px'
    } }
  })

  function down(e) {
    if (!e.isPrimary || e.button !== 0) return
    suppressClick = false
    pointer = { id: e.pointerId, x: e.clientX, y: e.clientY, at: [...fraction.value] }
    button.value.setPointerCapture(e.pointerId)
  }
  function move(e) {
    if (pointer?.id !== e.pointerId) return
    const mx = e.clientX - pointer.x, my = e.clientY - pointer.y
    if (!dragging.value && Math.hypot(mx, my) < 6) return
    dragging.value = true
    const { dx, dy } = geometry.value
    fraction.value = [clamp(pointer.at[0] + mx / (dx || 1), 0, 1), clamp(pointer.at[1] + my / (dy || 1), 0, 1)]
  }
  function end(e) {
    if (pointer?.id !== e.pointerId) return
    suppressClick = dragging.value
    if (dragging.value) {
      try { localStorage.setItem(storageKey, JSON.stringify(fraction.value)) } catch { /* 存储受限时仍可拖动。 */ }
    }
    dragging.value = false
    pointer = null
  }
  function wasDrag(e) { return e.detail !== 0 && suppressClick }

  onMounted(() => {
    try {
      const saved = JSON.parse(localStorage.getItem(storageKey))
      if (Array.isArray(saved) && saved.length === 2 && saved.every(n => Number.isFinite(n) && n >= 0 && n <= 1)) fraction.value = saved
    } catch { /* 忽略不可用或损坏的位置。 */ }
    measure()
    window.addEventListener('resize', measure)
    window.visualViewport?.addEventListener('resize', measure)
    window.visualViewport?.addEventListener('scroll', measure)
  })
  onUnmounted(() => {
    window.removeEventListener('resize', measure)
    window.visualViewport?.removeEventListener('resize', measure)
    window.visualViewport?.removeEventListener('scroll', measure)
  })
  return { geometry, dragging, down, move, end, wasDrag }
}
