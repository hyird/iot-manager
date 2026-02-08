/**
 * 资源型 Service 抽象
 * 统一约束 list/create/update/remove 接口形态，便于模块间保持一致。
 */

/** 资源 ID */
export type ResourceId = number;

/** 保存型输入，约定通过可选 id 区分创建或更新 */
export type ResourceSaveInput<TPayload> = TPayload & {
  id?: ResourceId;
};

/** 只读列表资源 */
export interface ListResourceApi<TListResult, TQuery = void> {
  list: (query?: TQuery) => Promise<TListResult>;
}

/** 标准 CRUD 资源 */
export interface CrudResourceApi<
  TListResult,
  TCreatePayload,
  TUpdatePayload = TCreatePayload,
  TCreateResult = void,
  TQuery = void,
> extends ListResourceApi<TListResult, TQuery> {
  create: (payload: TCreatePayload) => Promise<TCreateResult>;
  update: (id: ResourceId, payload: TUpdatePayload) => Promise<void>;
  remove: (id: ResourceId) => Promise<void>;
}

/** 定义只读资源，确保导出接口保持统一形态 */
export function defineListResource<TListResult, TQuery = void>(
  resource: ListResourceApi<TListResult, TQuery>
) {
  return resource;
}

/** 定义 CRUD 资源，确保导出接口保持统一形态 */
export function defineCrudResource<
  TListResult,
  TCreatePayload,
  TUpdatePayload = TCreatePayload,
  TCreateResult = void,
  TQuery = void,
>(resource: CrudResourceApi<TListResult, TCreatePayload, TUpdatePayload, TCreateResult, TQuery>) {
  return resource;
}
