"use client";

import { SchemaBrowser } from "@/components/SchemaBrowser";
import { SchemaDetails } from "@/components/SchemaDetails";
import { useState } from "react";
import type { SelectedItem } from "@/lib/types";

export default function Home() {
  const [selected, setSelected] = useState<SelectedItem | null>(null);

  return (
    <div className="flex h-screen">
      {/* Left panel: Schema Browser */}
      <div className="w-80 border-r border-gray-800 flex flex-col overflow-hidden">
        <div className="p-3 border-b border-gray-800 flex items-center gap-2">
          <span className="text-sm font-semibold text-gray-100">GioDB</span>
          <span className="text-xs text-gray-500">Admin</span>
        </div>
        <SchemaBrowser onSelect={setSelected} />
      </div>

      {/* Right panel: Details */}
      <div className="flex-1 overflow-auto">
        {selected ? (
          <SchemaDetails item={selected} />
        ) : (
          <div className="flex items-center justify-center h-full text-gray-600">
            <p>Select an item from the schema browser</p>
          </div>
        )}
      </div>
    </div>
  );
}
