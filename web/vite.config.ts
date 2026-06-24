import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { readFileSync, writeFileSync } from 'fs';
import { resolve } from 'path';

// ... (rest of file with escapeInlineScriptTags)

/**
 * Plugin: escape inline script content to prevent HTML parser confusion.
 *
 * vite-plugin-singlefile only escapes </script> → \x3C/script>, but does NOT
 * escape <script> appearing inside JS string literals (e.g. React's SSR
 * compatibility code: innerHTML="<script><\/script>").
 *
 * HTML parsers terminate a <script> block at the FIRST occurrence of
 * </script> — but some also choke on a bare <script> appearing inside a
 * string.  We replace literal <script (case-insensitive, inside script
 * content) with \x3Cscript so the HTML parser doesn't see a new
 * script open tag.
 *
 * Must run AFTER vite-plugin-singlefile (which runs in generateBundle),
 * so we use closeBundle to post-process the written HTML file.
 */
function escapeInlineScriptTags(): import('vite').Plugin {
  let outDir = 'dist';
  return {
    name: 'escape-inline-script-tags',
    enforce: 'post',
    configResolved(config) {
      outDir = config.build.outDir;
    },
    closeBundle() {
      const indexPath = resolve(outDir, 'index.html');
      let html = readFileSync(indexPath, 'utf-8');

      html = html.replace(
        /(<script[^>]*>)([\s\S]*?)(<\/script>)/gi,
        (_match: string, open: string, body: string, close: string) => {
          // Escape <script and </script inside the script body
          const safe = body
            .replace(/<(\/?)script/gi, '\\x3C$1script')
            // Also escape <!-- which can trigger HTML comment inside <script>
            .replace(/<!--/g, '\\x3C!--');
          return open + safe + close;
        },
      );

      writeFileSync(indexPath, html, 'utf-8');
    },
  };
}

export default defineConfig({
  plugins: [react(), viteSingleFile(), escapeInlineScriptTags()],
  build: {
    target: 'es2020',
    minify: 'esbuild',
    cssMinify: true,
    assetsInlineLimit: 100000,
  },
});
