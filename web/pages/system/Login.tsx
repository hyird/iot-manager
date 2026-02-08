import { useMutation } from "@tanstack/react-query";
import { Button, Card, Form, Input, Typography } from "antd";
import { useEffect, useRef } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import { authApi } from "@/services";
import { useAuthStore } from "@/store/hooks";
import type { Auth } from "@/types";

interface LocationState {
  from?: {
    pathname: string;
  };
}

interface Particle {
  x: number;
  y: number;
  size: number;
  speedX: number;
  speedY: number;
  opacity: number;
}

function createParticle(width: number, height: number): Particle {
  return {
    x: Math.random() * width,
    y: Math.random() * height,
    size: Math.random() * 3 + 1,
    speedX: Math.random() * 1 - 0.5,
    speedY: Math.random() * 1 - 0.5,
    opacity: Math.random() * 0.5 + 0.2,
  };
}

function updateParticle(p: Particle, width: number, height: number): void {
  p.x += p.speedX;
  p.y += p.speedY;
  if (p.x > width) p.x = 0;
  if (p.x < 0) p.x = width;
  if (p.y > height) p.y = 0;
  if (p.y < 0) p.y = height;
}

function drawParticle(ctx: CanvasRenderingContext2D, p: Particle): void {
  ctx.fillStyle = `rgba(100, 120, 180, ${p.opacity})`;
  ctx.beginPath();
  ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
  ctx.fill();
}

const { Title } = Typography;

// 提取样式对象到组件外部，避免每次渲染重新创建
const containerStyle = {
  height: "100vh",
  width: "100%",
  display: "flex",
  alignItems: "center",
  justifyContent: "center",
  background: "linear-gradient(-45deg, #e0eafc, #cfdef3, #a8c0ff, #e0eafc)",
  backgroundSize: "400% 400%",
  animation: "gradientShift 15s ease infinite",
  position: "relative" as const,
  overflow: "hidden" as const,
};

const canvasStyle = {
  position: "absolute" as const,
  top: 0,
  left: 0,
  width: "100%",
  height: "100%",
  pointerEvents: "none" as const,
};

const cardStyle = {
  width: "min(360px, calc(100vw - 32px))",
  boxShadow: "0 4px 24px rgba(0, 0, 0, 0.1)",
  position: "relative" as const,
  zIndex: 1,
};

const titleStyle = { textAlign: "center" as const, marginBottom: 24 };

export function LoginPage() {
  const [form] = Form.useForm<Auth.LoginRequest>();
  const navigate = useNavigate();
  const location = useLocation();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const { token, setAuth } = useAuthStore();

  const mutation = useMutation({
    mutationFn: authApi.login,
    onSuccess: (data) => {
      setAuth(data.token, data.refreshToken, data.user);
    },
  });

  // 登录成功后自动跳转
  useEffect(() => {
    if (token && !mutation.isPending) {
      const from = (location.state as LocationState)?.from?.pathname || "/home";
      navigate(from, { replace: true });
    }
  }, [token, mutation.isPending, location.state, navigate]);

  const onFinish = (values: Auth.LoginRequest) => {
    // 防止重复提交
    if (mutation.isPending) return;
    mutation.mutate(values);
  };

  // 粒子动画
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    let animationId: number;
    let particles: Particle[] = [];

    const resize = () => {
      canvas.width = window.innerWidth;
      canvas.height = window.innerHeight;
    };

    const init = () => {
      particles = [];
      const particleCount = Math.floor((canvas.width * canvas.height) / 15000);
      for (let i = 0; i < particleCount; i++) {
        particles.push(createParticle(canvas.width, canvas.height));
      }
    };

    const connectParticles = () => {
      for (let i = 0; i < particles.length; i++) {
        for (let j = i + 1; j < particles.length; j++) {
          const dx = particles[i].x - particles[j].x;
          const dy = particles[i].y - particles[j].y;
          const distance = Math.sqrt(dx * dx + dy * dy);

          if (distance < 120) {
            ctx.strokeStyle = `rgba(100, 120, 180, ${0.2 * (1 - distance / 120)})`;
            ctx.lineWidth = 0.5;
            ctx.beginPath();
            ctx.moveTo(particles[i].x, particles[i].y);
            ctx.lineTo(particles[j].x, particles[j].y);
            ctx.stroke();
          }
        }
      }
    };

    const animate = () => {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      particles.forEach((p) => {
        updateParticle(p, canvas.width, canvas.height);
        drawParticle(ctx, p);
      });
      connectParticles();
      animationId = requestAnimationFrame(animate);
    };

    resize();
    init();
    animate();

    const handleResize = () => {
      resize();
      init();
    };
    window.addEventListener("resize", handleResize);

    return () => {
      cancelAnimationFrame(animationId);
      window.removeEventListener("resize", handleResize);
    };
  }, []);

  return (
    <>
      <style>{`
        @keyframes gradientShift {
          0% { background-position: 0% 50%; }
          50% { background-position: 100% 50%; }
          100% { background-position: 0% 50%; }
        }
      `}</style>
      <div style={containerStyle}>
        <canvas ref={canvasRef} style={canvasStyle} />
        <Card style={cardStyle}>
          <Title level={3} style={titleStyle}>
            登录
          </Title>
          <Form<Auth.LoginRequest>
            form={form}
            layout="vertical"
            onFinish={onFinish}
          >
            <Form.Item
              label="用户名"
              name="username"
              rules={[{ required: true, message: "请输入用户名" }]}
            >
              <Input placeholder="admin" />
            </Form.Item>
            <Form.Item
              label="密码"
              name="password"
              rules={[{ required: true, message: "请输入密码" }]}
            >
              <Input.Password placeholder="123456" />
            </Form.Item>
            <Form.Item>
              <Button
                type="primary"
                htmlType="submit"
                block
                loading={mutation.isPending}
                disabled={mutation.isPending}
              >
                登录
              </Button>
            </Form.Item>
          </Form>
        </Card>
      </div>
    </>
  );
}

export default LoginPage;
