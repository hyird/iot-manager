import { readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { gzip as gzipCallback } from "node:zlib";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";
import cssInjectedByJs from "vite-plugin-css-injected-by-js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const gzip = promisify(gzipCallback);

const gzipStaticAssets = () => ({
  name: "gzip-static-assets",
  apply: "build" as const,
  async closeBundle() {
    const outDir = path.resolve(__dirname, "build/web");
    const compressibleExtensions = new Set([".css", ".html", ".js", ".json", ".svg"]);
    const files = await readdir(outDir, { withFileTypes: true });

    await Promise.all(
      files
        .filter((file) => file.isFile() && compressibleExtensions.has(path.extname(file.name)))
        .map(async (file) => {
          const sourcePath = path.join(outDir, file.name);
          const source = await readFile(sourcePath);
          const compressed = await gzip(source, { level: 9 });
          await writeFile(`${sourcePath}.gz`, compressed);
        }),
    );
  },
});

export default defineConfig({
  root: "web",
  plugins: [react(), tailwindcss(), cssInjectedByJs(), gzipStaticAssets()],
  server: {
    port: 5173,
    proxy: {
      "/api": {
        target: "http://127.0.0.1:3000",
        changeOrigin: true,
      },
      "/ws": {
        target: "ws://127.0.0.1:3000",
        ws: true,
      },
    },
  },
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "web"),
    },
  },
  build: {
    target: "es6",
    outDir: "../build/web",
    emptyOutDir: true,
    chunkSizeWarningLimit: 10000,
    cssCodeSplit: false,
    assetsInlineLimit: () => true,
    modulePreload: false,
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
        // Intentionally disabled code-splitting to reduce HTTP requests and simplify deployment.
        // Trade-off: larger single bundle, but better for internal SPA with infrequent deployments.
        entryFileNames: "app-[hash].js",
      },
    },
  },
});
