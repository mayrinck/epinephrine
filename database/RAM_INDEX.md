# RAM Module Index

This file maps detected memory modules (from `dmidecode -t memory` Type 17 `Manufacturer` + `Part Number`) to their commercial/product-line names and official product pages.

When a module matches a row below, its product line name (first column) and official page URL become available in the application. Modules that are not present in this database keep the fallback name built from `Manufacturer` + `Part Number` and have no website button.

The product line name (e.g. "Loki" for the Asgard Loki series) comes from the module's retail box and cannot be read from any known command, so it is catalogued here manually.

To add a new entry, insert a row in the table below with the name as it should appear in the application, the `Manufacturer` exactly as reported by `sudo dmidecode -t memory`, the `Part Number` exactly as reported, and the URL to its official product page.

Details:

- `Part Number` is matched exactly (case-insensitive) against the module's SMBIOS value. Rows with an *empty* Part Number act as a manufacturer-level fallback: they are used only when no exact Part Number row matches, and there should be at most one such row per manufacturer.
- The `Official Product Page` URL is what the application opens from each stick's "Official Website" button.

## Table

| Memory Name | Manufacturer | Part Number | Official Product Page |
|---|---|---|---|
| Asgard Loki VMA45UG-MEC1U2AW2 | Asgard | VMA45UG-MEC1U2AW2 | https://www.asgardoc.com/ |
| Asgard Valkyrie DDR4 RGB | Asgard |  | https://www.asgardoc.com/?s3/35.html |