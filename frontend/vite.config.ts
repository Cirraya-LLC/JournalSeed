import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [sveltekit()],
  server: {
    port: 5173,
    strictPort: false,
    proxy: {
      '/api': 'http://127.0.0.1:8080'
    }
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.ts'],
    setupFiles: ['./src/test-setup.ts']
  }
});
