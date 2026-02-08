import { AnimatePresence, motion } from "framer-motion";
import { useLocation, useOutlet } from "react-router-dom";
import { fadeVariants, pageTransition } from "@/utils/animations";

/**
 * 根路由过渡动画组件 - 用于登录页面和后台布局之间的整页过渡
 *
 * 应该在路由配置的最外层使用
 */
export function RootTransition() {
  const outlet = useOutlet();
  const location = useLocation();

  // 只区分登录页面和后台应用，后台内部切换不触发整页动画
  const animationKey = location.pathname === "/login" ? "login" : "app";

  return (
    <AnimatePresence mode="wait" initial={false}>
      <motion.div
        key={animationKey}
        initial="initial"
        animate="animate"
        exit="exit"
        variants={fadeVariants}
        transition={pageTransition}
        className="w-full h-full"
      >
        {outlet}
      </motion.div>
    </AnimatePresence>
  );
}
