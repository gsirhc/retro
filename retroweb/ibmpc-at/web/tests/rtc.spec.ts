import { test } from "./fixtures";
import { boot, waitForScreen, typeStr } from "./helpers";

// The MC146818A keeps time from power-on, loaded with the visitor's local clock.

test("DOS boots to today's date from the RTC", async ({ page }) => {
  await boot(page);
  await typeStr(page, "DATE");
  const now = new Date();
  const mm = String(now.getMonth() + 1).padStart(2, "0");
  const dd = String(now.getDate()).padStart(2, "0");
  await waitForScreen(page, new RegExp(`${mm}.${dd}.${now.getFullYear()}`), 10_000);
});
