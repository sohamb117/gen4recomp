import type { ReactNode } from "react";
export function Panel({
  title,
  children,
  className = "",
  aside,
}: {
  title: string;
  children: ReactNode;
  className?: string;
  aside?: ReactNode;
}) {
  return (
    <section className={`panel ${className}`}>
      <header className="panel-bar">
        <span>{title}</span>
        <span className="panel-tools">
          {aside}
          <i />
          <i />
          <i />
          <b aria-hidden="true">−</b>
        </span>
      </header>
      {children}
    </section>
  );
}
