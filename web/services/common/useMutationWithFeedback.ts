/**
 * 带反馈的通用 Mutation Hook
 * 统一处理成功/错误提示和缓存失效
 */

import { type QueryKey, useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";

interface MutationWithFeedbackOptions<TData, TVariables> {
  /** 变更函数 */
  mutationFn: (variables: TVariables) => Promise<TData>;
  /** 成功提示，支持函数动态生成 */
  successMessage?: string | ((data: TData, variables: TVariables) => string);
  /** 错误提示，支持函数动态生成 */
  errorMessage?: string | ((error: Error) => string);
  /** 成功后需要失效的缓存键 */
  invalidateKeys?: QueryKey[];
  /** 成功回调（在提示和缓存失效之后） */
  onSuccess?: (data: TData, variables: TVariables) => void;
  /** 错误回调（在提示之后） */
  onError?: (error: Error, variables: TVariables) => void;
}

export function useMutationWithFeedback<TData = void, TVariables = void>(
  options: MutationWithFeedbackOptions<TData, TVariables>
) {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: options.mutationFn,
    onSuccess: (data, variables) => {
      if (options.successMessage) {
        const msg =
          typeof options.successMessage === "function"
            ? options.successMessage(data, variables)
            : options.successMessage;
        message.success(msg);
      }

      options.invalidateKeys?.forEach((key) => {
        queryClient.invalidateQueries({ queryKey: key });
      });

      options.onSuccess?.(data, variables);
    },
    onError: (error: Error, variables) => {
      // 自定义错误提示覆盖拦截器的默认提示
      if (options.errorMessage) {
        const msg =
          typeof options.errorMessage === "function"
            ? options.errorMessage(error)
            : options.errorMessage;
        message.error(msg);
      }
      // 未设置 errorMessage 时，由 Axios 拦截器统一处理错误提示

      options.onError?.(error, variables);
    },
  });
}
