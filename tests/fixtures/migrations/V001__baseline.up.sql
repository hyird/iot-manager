-- V001: 基线迁移
-- 将 DatabaseInitializer 管理的现有 schema 注册为 V001
-- 对于全新数据库，此文件包含完整的 schema 创建 SQL
-- 对于已有数据库（DatabaseInitializer 已初始化），此文件为校验性 no-op
--
-- 生成时间: 2026-03-17
-- 生成方式: pg_dump --schema-only --no-owner iot_manager_dev

-- ==================== 枚举类型 ====================
DO $$ BEGIN
    CREATE TYPE status_enum AS ENUM ('enabled', 'disabled');
EXCEPTION
    WHEN duplicate_object THEN null;
END $$;

DO $$ BEGIN
    CREATE TYPE menu_type_enum AS ENUM ('menu', 'page', 'button');
EXCEPTION
    WHEN duplicate_object THEN null;
END $$;

-- ==================== 系统表 ====================
CREATE TABLE IF NOT EXISTS sys_menu (
    id SERIAL PRIMARY KEY,
    name VARCHAR(50) NOT NULL,
    parent_id INT DEFAULT 0,
    type menu_type_enum DEFAULT 'page',
    path VARCHAR(255),
    component VARCHAR(255),
    permission_code VARCHAR(100),
    icon VARCHAR(100),
    is_default BOOLEAN DEFAULT FALSE,
    status status_enum DEFAULT 'enabled',
    sort_order INT DEFAULT 0,
    created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMPTZ NULL
);

CREATE TABLE IF NOT EXISTS sys_role (
    id SERIAL PRIMARY KEY,
    name VARCHAR(50) NOT NULL,
    code VARCHAR(50) NOT NULL,
    description VARCHAR(255),
    status status_enum DEFAULT 'enabled',
    sort_order INT DEFAULT 0,
    created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMPTZ NULL
);

CREATE TABLE IF NOT EXISTS sys_user (
    id SERIAL PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    nickname VARCHAR(100),
    email VARCHAR(100),
    phone VARCHAR(20),
    avatar VARCHAR(255),
    status status_enum DEFAULT 'enabled',
    department_id INT,
    created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMPTZ NULL
);

-- ==================== 索引 ====================
CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_role_code_active
    ON sys_role (code) WHERE deleted_at IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS idx_sys_user_username_active
    ON sys_user (username) WHERE deleted_at IS NULL;

-- ==================== 基线校验 ====================
-- 确保关键表存在（用于检测损坏的基线状态）
DO $$ BEGIN
    ASSERT EXISTS (
        SELECT 1 FROM information_schema.tables
        WHERE table_name = 'sys_user'
    ), 'Baseline validation failed: sys_user table missing';
END $$;
