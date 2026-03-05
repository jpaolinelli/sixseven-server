import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "SixSevenDB Admin",
  description: "SixSevenDB Web Administration Console",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en">
      <body className="bg-gray-950 text-gray-200 h-screen overflow-hidden">
        {children}
      </body>
    </html>
  );
}
