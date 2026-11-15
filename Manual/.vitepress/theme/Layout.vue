<script setup>
import DefaultTheme from 'vitepress/theme'
import { useData } from 'vitepress'
import Guide from './Guide.vue'

const { theme, localeIndex, page } = useData()
</script>

<template>
  <DefaultTheme.Layout>
    <template #nav-bar-content-after>
      <a class="flare-repo" :href="theme.repository" :aria-label="theme.guide.Repository" :title="theme.guide.Repository">
        <span class="vpi-arrow-left" aria-hidden="true" />
        <span class="full-label">{{ theme.guide.Repository }}</span><span class="short-label">GitHub</span>
      </a>
    </template>
    <template #layout-bottom>
      <ClientOnly><Guide v-if="localeIndex === 'root' && !page.isNotFound" /></ClientOnly>
    </template>
  </DefaultTheme.Layout>
</template>

<style scoped>
.flare-repo { display: inline-flex; align-items: center; gap: 5px; padding: 5px 10px; margin-inline: 8px; border: 1px solid var(--vp-c-divider); border-radius: 8px; font-size: 13px; font-weight: 600; }
.flare-repo:hover, .flare-repo:focus-visible { color: var(--vp-c-brand-1); border-color: currentColor; }
.short-label { display: none; }
@media (max-width: 540px) {
  .flare-repo { padding: 5px; margin-inline: 0; gap: 2px; }
  .full-label { display: none; }
  .short-label { display: inline; }
}
</style>
