import json
import re
import csv
import os

def extract_symbols(log_file_path, output_json_path, output_csv_path):
    symbols = []
    # Regex to capture ID, Name, and Description from the message string
    # Example message: "Symbol ID: 21508 Name: APPLE Desc: "
    # Example message: "Symbol ID: 22233 Name: CHINA OVERSEAS LAND & INVEST Desc: "
    pattern = re.compile(r"Symbol ID:\s*(\d+)\s*Name:\s*(.*?)\s*Desc:\s*(.*)")

    if not os.path.exists(log_file_path):
        print(f"Error: Log file not found at {log_file_path}")
        return

    print(f"Reading log file: {log_file_path}")
    
    with open(log_file_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            
            try:
                # The log line itself is a JSON object
                log_entry = json.loads(line)
                message = log_entry.get('message', '')
                
                if "Symbol ID:" in message:
                    match = pattern.search(message)
                    if match:
                        symbol_id = match.group(1).strip()
                        name = match.group(2).strip()
                        desc = match.group(3).strip()
                        
                        symbols.append({
                            "id": symbol_id,
                            "name": name,
                            "description": desc
                        })
            except json.JSONDecodeError:
                # Fallback for non-JSON lines if any (e.g. raw text logs)
                if "Symbol ID:" in line:
                    match = pattern.search(line)
                    if match:
                        symbol_id = match.group(1).strip()
                        name = match.group(2).strip()
                        desc = match.group(3).strip()
                        symbols.append({
                            "id": symbol_id,
                            "name": name,
                            "description": desc
                        })

    # Remove duplicates based on ID
    unique_symbols = {s['id']: s for s in symbols}.values()
    sorted_symbols = sorted(unique_symbols, key=lambda x: int(x['id']))

    print(f"Found {len(sorted_symbols)} unique symbols.")

    # Write to JSON
    with open(output_json_path, 'w') as f:
        json.dump(list(sorted_symbols), f, indent=2)
    print(f"Wrote JSON to {output_json_path}")

    # Write to CSV
    with open(output_csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(["ID", "Name", "Description"])
        for sym in sorted_symbols:
            writer.writerow([sym['id'], sym['name'], sym['description']])
    print(f"Wrote CSV to {output_csv_path}")

if __name__ == "__main__":
    extract_symbols("orderbook.log", "symbols.json", "symbols.csv")
