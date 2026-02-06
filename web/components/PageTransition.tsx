import { AnimatePresence, motion } from "framer-motion";
import { useLocation, useOutlet } from "react-router-dom";
import { fastTransition, slideUpVariants } from "@/utils/animations";

/**
 * 页面过渡动画组件 - 用于路由切换时的内容过渡
 *
 * 应该在布局组件中替代 <Outlet /> 使用
 *
 * @example
 * // 在 AdminLayout 中使用
 * <Content>
 *   <PageTransition />
 * </Content>
 */
export function PageTransition() {
  const outlet = useOutlet();
  const location = useLocation();

  return (
    <AnimatePresence mode="wait" initial={false}>
      <motion.div
        key={location.pathname}
        initial="initial"
        animate="animate"
        exit="exit"
        variants={slideUpVariants}
        transition={fastTransition}
        className="w-full h-full"
      >
        {outlet}
      </motion.div>
    </AnimatePresence>
  );
}
