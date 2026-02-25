"use client";

import { useState, type ReactNode } from "react";

interface TreeNodeProps {
  label: string;
  icon?: string;
  dimmed?: boolean;
  expandable?: boolean;
  defaultExpanded?: boolean;
  children?: ReactNode;
  onClick?: () => void;
  badge?: string;
}

export function TreeNode({
  label,
  icon,
  dimmed,
  expandable,
  defaultExpanded = false,
  children,
  onClick,
  badge,
}: TreeNodeProps) {
  const [expanded, setExpanded] = useState(defaultExpanded);

  const handleClick = () => {
    if (expandable) setExpanded(!expanded);
    onClick?.();
  };

  return (
    <div>
      <div
        className={`flex items-center gap-1.5 px-2 py-0.5 cursor-pointer hover:bg-gray-800/50 rounded text-sm select-none ${
          dimmed ? "text-gray-500 italic" : "text-gray-300"
        }`}
        onClick={handleClick}
      >
        {expandable && (
          <span className="w-4 text-center text-xs text-gray-500">
            {expanded ? "▾" : "▸"}
          </span>
        )}
        {!expandable && <span className="w-4" />}
        {icon && <span className="text-xs">{icon}</span>}
        <span className="truncate flex-1">{label}</span>
        {badge && (
          <span className="text-[10px] px-1 py-0.5 rounded bg-gray-800 text-gray-500">
            {badge}
          </span>
        )}
      </div>
      {expanded && children && (
        <div className="ml-3 border-l border-gray-800/50">{children}</div>
      )}
    </div>
  );
}
