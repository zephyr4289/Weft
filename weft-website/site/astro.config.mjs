import { defineConfig } from 'astro/config';

export default defineConfig({
  site: 'https://zephyr4289.github.io',
  base: '/Weft',
  trailingSlash: 'ignore',
  output: 'static',
  build: { inlineStylesheets: 'auto' },
});
