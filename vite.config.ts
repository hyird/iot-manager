import path from "node:path";
import { fileURLToPath } from "node:url";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";
import cssInjectedByJs from "vite-plugin-css-injected-by-js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export default defineConfig({
  root: "web",
  plugins: [react(), tailwindcss(), cssInjectedByJs()],
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
    chunkSizeWarningLimit: 5000,
    cssCodeSplit: false,
    assetsInlineLimit: () => true,
    modulePreload: false,
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
        // Intentionally disabled code-splitting to reduce HTTP requests and simplify deployment.
        // Trade-off: larger single bundle, but better for internal SPA with infrequent deployments.
        entryFileNames: "app.js",
      },
    },
  },
});
