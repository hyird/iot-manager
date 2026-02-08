import { useCallback, useEffect, useRef } from "react";

/**
 * 函数防抖 Hook，替代 ahooks 的 useDebounceFn
 */
// biome-ignore lint/suspicious/noExplicitAny: 需要兼容任意函数签名
export function useDebounceFn<T extends (...args: any[]) => any>(
  fn: T,
  wait = 300
): { run: (...args: Parameters<T>) => void; cancel: () => void } {
  const timerRef = useRef<ReturnType<typeof setTimeout>>(null);
  const fnRef = useRef(fn);
  fnRef.current = fn;

  const cancel = useCallback(() => {
    if (timerRef.current) {
      clearTimeout(timerRef.current);
      timerRef.current = null;
    }
  }, []);

  const run = useCallback(
    (...args: Parameters<T>) => {
      cancel();
      timerRef.current = setTimeout(() => fnRef.current(...args), wait);
    },
    [cancel, wait]
  );

  useEffect(() => cancel, [cancel]);

  return { run, cancel };
}
