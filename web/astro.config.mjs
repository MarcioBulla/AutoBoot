import { defineConfig } from 'astro/config';
import vue from '@astrojs/vue';
import siteConfig from './site.config.json' with { type: 'json' };

export default defineConfig({
  site: `${siteConfig.scheme}://${siteConfig.domain}`,
  integrations: [vue()],
});
