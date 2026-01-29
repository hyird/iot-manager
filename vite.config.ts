import { defineConfig } from "vite";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { randomUUID } from "node:crypto";
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const genName = () => randomUUID();

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    port: 5173,
    proxy: {
      "/api": {
        target: "http://127.0.0.1:3000",
        changeOrigin: true,
      },
    },
  },
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "web"),
    },
  },
  build: {
    outDir: "dist/web",
    emptyOutDir: true,
    chunkSizeWarningLimit: 9999,
    rollupOptions: {
      output: {
        entryFileNames: () => `assets/${genName()}.js`,
        chunkFileNames: () => `assets/${genName()}.js`,
        assetFileNames: (assetInfo) => {
          const ext = assetInfo.name?.split(".").pop() || "bin";
          return `assets/${genName()}.${ext}`;
        },
        manualChunks(id) {
          if (id.includes("node_modules")) {
            // @tanstack/react-query 相关
            if (id.includes("@tanstack")) {
              return "vendor-query";
            }
            // ahooks
            if (id.includes("ahooks")) {
              return "vendor-ahooks";
            }
          }
        },
      },
    },
  },
});
