function normalizePath(path: string) {
  return path.startsWith("/") ? path : `/${path}`;
}

export function getHashRoutePath(path: string) {
  const baseUrl = (import.meta.env.BASE_URL || "/").replace(/\/$/, "");
  return `${baseUrl || ""}/#${normalizePath(path)}`;
}

export function redirectToLogin() {
  if (window.location.hash === "#/login") {
    return;
  }
  window.location.replace(getHashRoutePath("/login"));
}
