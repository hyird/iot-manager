import { motion, AnimatePresence } from "framer-motion";
import { useOutlet, useLocation } from "react-router-dom";
import { slideUpVariants, fastTransition } from "@/utils/animations";

const containerStyle = { width: "100%", height: "100%" };

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
        style={containerStyle}
      >
        {outlet}
      </motion.div>
    </AnimatePresence>
  );
}
