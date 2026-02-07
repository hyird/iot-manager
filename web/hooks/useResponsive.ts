import { Grid } from "antd";

const { useBreakpoint } = Grid;

/** 响应式断点 hook — 统一移动端/平板/桌面判断 */
export function useResponsive() {
  const screens = useBreakpoint();
  return {
    /** < 576px (手机竖屏) */
    isMobile: !screens.sm,
    /** 576–991px (平板/手机横屏) */
    isTablet: !!screens.sm && !screens.lg,
    /** >= 992px */
    isDesktop: !!screens.lg,
  };
}
