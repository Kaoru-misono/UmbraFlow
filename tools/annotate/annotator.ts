import { startAnnotator } from "./ui/app.mjs";

export type QueueCategory =
  | "proposals"
  | "conflicts"
  | "low-confidence"
  | "missing-identity"
  | "unsafe-actions"
  | "uncovered-frames"
  | "compile-replay"
  | "review-history";

export type AnnotatorAdapter = Parameters<typeof startAnnotator>[0]["adapter"];

export function bootAnnotator(root: HTMLElement, adapter?: AnnotatorAdapter) {
  return startAnnotator({ root, adapter });
}
