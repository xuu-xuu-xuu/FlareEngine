import indexes from '@localSearchIndex'
import MiniSearch from 'minisearch'

// VitePress 1.6 的本地索引以页面及章节为单位，id 已含部署路径和锚点。
export async function loadGuideSearch(locale, options) {
  const { default: json } = await indexes[locale]()
  return MiniSearch.loadJSON(json, {
    fields: ['title', 'titles', 'text'],
    storeFields: ['title', 'titles'],
    ...options.options,
    searchOptions: { fuzzy: 0.2, prefix: true, boost: { title: 4, text: 2, titles: 1 }, ...options.searchOptions }
  })
}
