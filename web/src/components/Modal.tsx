import { useEffect, useRef, type ReactNode } from "react";
import { X } from "lucide-react";
import { Panel } from "./Panel";

export function Modal({
  title,
  onClose,
  children,
}: {
  title: string;
  onClose: () => void;
  children: ReactNode;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const element = dialog.current!;
    element.showModal();
    return () => element.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="modal"
      aria-label={title}
      onCancel={(event) => {
        event.preventDefault();
        onClose();
      }}
      onClick={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <Panel
        title={title}
        aside={
          <button
            autoFocus
            aria-label={`Close ${title.toLowerCase()}`}
            onClick={onClose}
          >
            <X size={16} />
          </button>
        }
      >
        <div className="modal-content">{children}</div>
      </Panel>
    </dialog>
  );
}
