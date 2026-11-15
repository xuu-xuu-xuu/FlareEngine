<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, shallowRef, watch } from 'vue'
import { useData, useRoute, useRouter } from 'vitepress'
import portrait from './flare-guide.webp'
import lids from './flare-guide-lids.svg'
import { useGuideFeedback } from './useGuideFeedback'
import { useGuidePosition } from './useGuidePosition'

const { theme, localeIndex, site } = useData()
const route = useRoute(), router = useRouter()
const copy = computed(() => theme.value.guide)
const modal = computed(() => theme.value.search.options.translations.modal)
const root = ref(), button = ref(), input = ref()
const { geometry, dragging, pressed, down, move, end, ignoreClick } = useGuidePosition(button, 'flare:guide-position:' + site.value.base)
const { squashed, press, release } = useGuideFeedback()
function startPress(e) {
  down(e)
  if (e.isPrimary && e.button === 0 && pressed.value) press()
}
function finishPress(e) {
  const wasPressed = pressed.value
  const tap = end(e)
  if (wasPressed && !pressed.value) release(e.type !== 'pointerup')
  if (tap) toggle()
}
function activate(e) {
  if (ignoreClick(e)) return
  if (e.detail === 0) { press(); release() }
  toggle(e.detail === 0)
}
const open = ref(false), query = ref(''), index = shallowRef(), state = ref('idle')
const results = computed(() => query.value.trim() && index.value ? index.value.search(query.value.trim()).slice(0, 8) : [])

async function load() {
  if (index.value || state.value === 'loading') return
  state.value = 'loading'
  try {
    const { loadGuideSearch } = await import('./guideSearch')
    index.value = await loadGuideSearch(localeIndex.value, theme.value.search.options.miniSearch)
    state.value = 'ready'
  } catch { state.value = 'error' }
}
function close(restoreFocus = false) {
  open.value = false
  if (restoreFocus) button.value?.focus({ preventScroll: true })
}
function toggle(fromKeyboard = false) {
  if (open.value) return close(fromKeyboard)
  open.value = true
  nextTick(() => input.value?.focus({ preventScroll: true }))
  load()
}
function outside(e) {
  if (open.value && !root.value?.contains(e.target)) close()
}
function enter(e) {
  if (e.isComposing || !results.value.length) return
  e.preventDefault()
  router.go(results.value[0].id)
  close(true)
}
function suggest(term) {
  query.value = term
  input.value.focus({ preventScroll: true })
}
watch(() => route.path, () => close())
onMounted(() => {
  document.addEventListener('pointerdown', outside)
  document.addEventListener('focusin', outside)
})
onUnmounted(() => {
  document.removeEventListener('pointerdown', outside)
  document.removeEventListener('focusin', outside)
})
</script>

<template>
  <div ref="root" class="flare-guide" :style="{ left: geometry.x + 'px', top: geometry.y + 'px' }"
    @keydown.esc.stop.prevent="close(true)">
    <button ref="button" type="button" class="portrait" :class="{ dragging, squashed }" :title="copy.Launch"
      :aria-label="open ? modal.backButtonTitle : copy.Badge" :aria-expanded="open" aria-controls="flare-guide-panel"
      @pointerdown="startPress" @pointermove="move" @pointerup="finishPress"
      @pointercancel="finishPress" @lostpointercapture="finishPress" @dragstart.prevent @click="activate">
      <span class="squash"><span class="idle">
        <img :src="portrait" width="456" height="559" alt="" draggable="false">
        <img class="lids" :src="lids" width="456" height="559" alt="" draggable="false">
      </span></span>
    </button>
    <section v-if="open" id="flare-guide-panel" class="bubble" :class="{ above: geometry.above }" :style="geometry.panel"
      :aria-label="copy.Title">
      <div class="card">
        <header><strong><span aria-hidden="true">✦</span> {{ copy.Title }}</strong>
          <button type="button" class="close" :aria-label="modal.backButtonTitle" @click="close($event.detail === 0)">×</button>
        </header>
        <label class="field"><i class="vpi-search" aria-hidden="true" />
          <input ref="input" v-model="query" type="search" maxlength="64" autocomplete="off" enterkeyhint="search"
            :aria-label="theme.search.options.translations.button.buttonText" :placeholder="copy.Placeholder"
            @keydown.enter="enter" @keydown.down.prevent="root.querySelector('.hits a')?.focus()">
        </label>
        <div v-if="state === 'error'" class="notice" role="status">{{ copy.Unavailable }}
          <button type="button" @click="load">{{ copy.Retry }}</button>
        </div>
        <div v-else-if="!query.trim()" class="suggestions"><span>{{ copy.Try }}</span>
          <button v-for="term in ['D3D12', 'FBX', 'C#']" :key="term" type="button" @click="suggest(term)">{{ term }}</button>
        </div>
        <p v-else-if="!index || !results.length" class="notice" role="status">{{ !index ? copy.Loading : modal.noResultsText }}</p>
        <ul v-else class="hits">
          <li v-for="hit in results" :key="hit.id">
            <a :href="hit.id" @click="close()">
              <small v-if="hit.titles.length">{{ hit.titles.join(' › ') }}</small><span>{{ hit.title }}</span>
            </a>
          </li>
        </ul>
      </div>
    </section>
  </div>
</template>

<style scoped>
.flare-guide { position: fixed; z-index: calc(var(--vp-z-index-nav) - 1); }
.portrait {
  display: block;
  width: min(152px, 25dvh);
  padding: 0;
  background: transparent;
  cursor: grab;
  touch-action: none;
  user-select: none;
  -webkit-user-select: none;
}
.portrait.dragging { cursor: grabbing; }
.portrait { -webkit-tap-highlight-color: transparent; }
.portrait:focus { outline: none; }
.portrait:focus-visible::after {
  content: attr(aria-label);
  position: absolute;
  bottom: 0;
  left: 50%;
  transform: translateX(-50%);
  padding: 0 6px;
  border-radius: 4px;
  background: var(--vp-c-bg);
  color: var(--vp-c-text-1);
  font-size: 12px;
  white-space: nowrap;
}
.portrait img { display: block; width: 100%; height: auto; pointer-events: none; }
.squash, .idle {
  display: block;
  transform-origin: center bottom;
  pointer-events: none;
}
.squash { transition: transform 220ms cubic-bezier(.34, 1.56, .64, 1); }
.squashed .squash { transform: scale(1.05, .88); transition: transform 65ms ease-out; }
.idle { position: relative; animation: breathe 3.8s ease-in-out infinite; }
.lids { position: absolute; inset: 0; opacity: 0; animation: blink 6.7s linear infinite; }
@keyframes breathe { 50% { transform: scale(1.007, 1.018); } }
@keyframes blink {
  0%, 64%, 66.7%, 100% { opacity: 0; }
  64.6%, 66% { opacity: 1; }
}
@media (prefers-reduced-motion: reduce) {
  .portrait .squash { transition: none; }
  .idle, .lids { animation: none; }
}
.bubble { position: absolute; color: var(--vp-c-text-1); --edge: #79624c; --card: #252428; }
.bubble::before { content: ''; position: absolute; left: var(--tip); top: -5px; width: 10px; height: 10px; background: var(--card); border: solid var(--edge); border-width: 1px 0 0 1px; transform: translateX(-50%) rotate(45deg); }
.bubble.above::before { top: auto; bottom: -5px; transform: translateX(-50%) rotate(225deg); }
.card { max-height: var(--panel-height); overflow: auto; overscroll-behavior: contain; padding: 13px; border: 1px solid var(--edge); border-radius: 14px; background: var(--card); box-shadow: 0 10px 32px #0006; }
header, .field, .suggestions { display: flex; align-items: center; }
header { justify-content: space-between; margin-bottom: 9px; }
header strong { font-size: 14px; }
header strong span, .field i { color: var(--vp-c-brand-1); }
.close { width: 28px; height: 28px; border-radius: 6px; color: var(--vp-c-text-2); font-size: 21px; line-height: 1; }
.close:hover, .hits a:hover, .hits a:focus-visible { background: var(--vp-c-brand-soft); }
.field { gap: 8px; padding: 7px 10px; border: 1px solid var(--vp-c-divider); border-radius: 8px; background: var(--vp-c-bg); }
.field:focus-within { border-color: var(--vp-c-brand-1); }
.field input { min-width: 0; flex: 1; outline: none; font: inherit; font-size: 16px; }
.suggestions, .notice { margin-top: 10px; font-size: 12px; color: var(--vp-c-text-2); }
.suggestions { flex-wrap: wrap; gap: 7px; }
.suggestions button, .notice button { padding: 0 7px; border: 1px solid var(--vp-c-divider); border-radius: 6px; color: var(--vp-c-brand-1); }
.hits { margin-top: 9px; }
.hits a { display: grid; padding: 6px 8px; border-radius: 7px; font-size: 14px; line-height: 1.45; overflow-wrap: anywhere; }
.hits small { font-size: 11px; color: var(--vp-c-text-3); }
.bubble button:focus-visible, .hits a:focus-visible { outline: 2px solid var(--vp-c-brand-1); outline-offset: 2px; }
@media (max-width: 640px) { .portrait { width: min(112px, 25dvh); } }
</style>
