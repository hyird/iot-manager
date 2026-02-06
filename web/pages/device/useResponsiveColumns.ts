/**
 * 响应式列数 Hook
 */

import { useEffect, useState } from "react";

export function useResponsiveColumns(breakpoints = { sm: 768, lg: 1199 }): number {
  const [columns, setColumns] = useState(3);

  useEffect(() => {
    const calcColumns = () => {
      const width = window.innerWidth;
      if (width <= breakpoints.sm) setColumns(1);
      else if (width <= breakpoints.lg) setColumns(2);
      else setColumns(3);
    };
    calcColumns();
    window.addEventListener("resize", calcColumns);
    return () => window.removeEventListener("resize", calcColumns);
  }, [breakpoints.sm, breakpoints.lg]);

  return columns;
}
