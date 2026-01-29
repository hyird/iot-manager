import js from "@eslint/js";
import globals from "globals";
import reactHooks from "eslint-plugin-react-hooks";
import reactRefresh from "eslint-plugin-react-refresh";
import tseslint from "typescript-eslint";
import prettier from "eslint-plugin-prettier";

/**
 * Web 前端项目 ESLint 配置
 * 基于 React 19 + TypeScript + Vite
 */
export default tseslint.config(
  // 忽略的文件和目录
  {
    ignores: ["dist/**", "node_modules/**", "build/**", ".vite/**", "coverage/**"],
  },

  // 扩展推荐配置
  js.configs.recommended,
  ...tseslint.configs.recommended,

  // 全局配置
  {
    languageOptions: {
      ecmaVersion: 2024,
      sourceType: "module",
      globals: {
        ...globals.browser,
        ...globals.es2024,
      },
    },
    plugins: {
      "react-hooks": reactHooks,
      "react-refresh": reactRefresh,
      prettier: prettier,
    },
    rules: {
      // TypeScript 规则
      "@typescript-eslint/no-explicit-any": "warn",
      "@typescript-eslint/no-unused-vars": [
        "error",
        {
          argsIgnorePattern: "^_",
          varsIgnorePattern: "^_",
        },
      ],
      "@typescript-eslint/no-namespace": "off",

      // React Hooks 规则
      ...reactHooks.configs.recommended.rules,

      // React Refresh 规则
      "react-refresh/only-export-components": ["warn", { allowConstantExport: true }],

      // 通用规则
      "no-console": [
        "warn",
        {
          allow: ["warn", "error"],
        },
      ],
      "no-debugger": "warn",
      "prefer-const": "error",
      "no-var": "error",

      // Prettier 集成
      "prettier/prettier": "warn",
    },
  }
);
