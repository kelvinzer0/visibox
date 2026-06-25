#!/usr/bin/env python3
"""
infisical-setup.py - Pull secrets from Infisical and configure Linux server
Usage: python3 infisical-setup.py [--env dev] [--dry-run]

Requires: infisical CLI, gh CLI (optional), curl
"""

import json, os, subprocess, sys, re, argparse

RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[1;33m'
CYAN = '\033[0;36m'
BOLD = '\033[1m'
NC = '\033[0m'

def header(title):
    print(f"\n{CYAN}{BOLD}{'━'*60}{NC}")
    print(f"{CYAN}{BOLD}  {title}{NC}")
    print(f"{CYAN}{BOLD}{'━'*60}{NC}\n")

def ok(msg):   print(f"  {GREEN}✓{NC} {msg}")
def fail(msg): print(f"  {RED}✗{NC} {msg}")
def warn(msg): print(f"  {YELLOW}!{NC} {msg}")
def info(msg): print(f"  {CYAN}→{NC} {msg}")

def run(cmd, check=True, capture=True, timeout=30):
    try:
        r = subprocess.run(cmd, shell=True, check=check, capture_output=capture, text=True, timeout=timeout)
        return r.stdout.strip() if capture else None
    except subprocess.CalledProcessError as e:
        if capture and e.stderr:
            return e.stderr.strip()
        return None
    except subprocess.TimeoutExpired:
        return None

# ─── Config ───
INFISICAL_CLIENT_ID = os.environ.get("INFISICAL_CLIENT_ID", "")
INFISICAL_CLIENT_SECRET = os.environ.get("INFISICAL_CLIENT_SECRET", "")
INFISICAL_PROJECT_ID = os.environ.get("INFISICAL_PROJECT_ID", "")
INFISICAL_API_URL = os.environ.get("INFISICAL_API_URL", "https://app.infisical.com/api")
BASHRC = os.path.expanduser("~/.bashrc")

def check_prerequisites():
    header("Prerequisites")
    ok_flag = True

    if not run("which infisical", check=False):
        warn("infisical not found. Installing...")
        # Try npm
        if run("which npm", check=False):
            run("npm install -g @infisical/cli", check=False, timeout=60)
        if not run("which infisical", check=False):
            fail("Cannot install infisical. Install manually: npm i -g @infisical/cli")
            ok_flag = False
        else:
            ok(f"infisical installed: {run('infisical --version')}")
    else:
        ok(f"infisical: {run('infisical --version')}")

    if not INFISICAL_CLIENT_ID or not INFISICAL_CLIENT_SECRET:
        fail("Set INFISICAL_CLIENT_ID and INFISICAL_CLIENT_SECRET env vars")
        ok_flag = False
    else:
        ok(f"Client ID: {INFISICAL_CLIENT_ID[:8]}...")

    return ok_flag

def login():
    header("Infisical Login")
    result = run(
        f'infisical login --method=universal-auth '
        f'--client-id="{INFISICAL_CLIENT_ID}" '
        f'--client-secret="{INFISICAL_CLIENT_SECRET}" --silent'
    )
    if "Successfully authenticated" in (result or ""):
        ok("Authenticated with universal auth")
        # Extract token
        token_match = re.search(r'eyJ[A-Za-z0-9_-]+\.eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+', result or "")
        if token_match:
            return token_match.group(0)
    # Try to get token from CLI output
    result2 = run(
        f'infisical login --method=universal-auth '
        f'--client-id="{INFISICAL_CLIENT_ID}" '
        f'--client-secret="{INFISICAL_CLIENT_SECRET}"'
    )
    token_match = re.search(r'eyJ[A-Za-z0-9_-]+\.eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+', result2 or "")
    if token_match:
        ok("Got access token")
        return token_match.group(0)
    fail("Login failed")
    return None

def get_projects(token):
    header("List Projects")
    result = run(f'curl -s "{INFISICAL_API_URL}/v1/workspace" -H "Authorization: Bearer {token}"')
    try:
        data = json.loads(result)
        projects = data.get("workspaces", [])
        if not projects:
            fail("No projects found")
            return []
        for p in projects:
            envs = ", ".join([e.get("slug","") for e in p.get("environments", [])])
            print(f"  {BOLD}{p['name']}{NC} ({p['id'][:8]}...) envs: {envs}")
        return projects
    except:
        fail("Failed to list projects")
        return []

def get_secrets(token, project_id, env="dev"):
    header(f"Pull Secrets (env: {env})")
    result = run(
        f'curl -s "{INFISICAL_API_URL}/v3/secrets/raw?workspaceId={project_id}&environment={env}" '
        f'-H "Authorization: Bearer {token}"'
    )
    try:
        data = json.loads(result)
        secrets = data.get("secrets", [])
        if not secrets:
            warn(f"No secrets found in env '{env}'")
            return []
        ok(f"Found {len(secrets)} secrets")
        for s in secrets:
            key = s["secretKey"]
            val = s["secretValue"]
            comment = s.get("secretComment", "")
            masked = val[:4] + "***" + val[-4:] if len(val) > 8 else "***"
            desc = f" ({comment})" if comment else ""
            print(f"    {BOLD}{key}{NC} = {masked}{desc}")
        return secrets
    except:
        fail("Failed to fetch secrets")
        return []

def generate_env_script(secrets, env):
    """Generate bash export script from secrets"""
    lines = [
        f"# === Infisical Secrets ({env}) ===",
        f"# Auto-generated by infisical-setup.py",
        ""
    ]

    for s in secrets:
        key = s["secretKey"]
        val = s["secretValue"]
        comment = s.get("secretComment", "")
        if comment:
            lines.append(f"# {comment}")

        if "\n" in val:
            # Multiline: use heredoc
            lines.append(f'export {key}="$(cat << \'INFISICAL_EOF\'')
            lines.append(val)
            lines.append('INFISICAL_EOF')
            lines.append(')"')
        else:
            escaped = val.replace("'", "'\\''")
            lines.append(f"export {key}='{escaped}'")
        lines.append("")

    return "\n".join(lines)

def write_to_bashrc(env_script, env):
    """Append secrets to ~/.bashrc (replaces existing block)"""
    header("Write to ~/.bashrc")

    # Remove old infisical block
    if os.path.exists(BASHRC):
        with open(BASHRC) as f:
            content = f.read()
        content = re.sub(r'\n# === Infisical Secrets.*', '', content, flags=re.DOTALL)
        with open(BASHRC, 'w') as f:
            f.write(content.rstrip() + '\n')
        info("Cleaned old Infisical block from bashrc")

    with open(BASHRC, 'a') as f:
        f.write('\n' + env_script + '\n')

    ok(f"Appended secrets to {BASHRC}")

def setup_ssh_keys(secrets):
    """Write SSH keys from secrets to ~/.ssh/"""
    header("SSH Keys")
    ssh_dir = os.path.expanduser("~/.ssh")
    os.makedirs(ssh_dir, mode=0o700, exist_ok=True)

    key_map = {
        "SSH_ED25519_PRIV": ("id_ed25519", 0o600),
        "SSH_ED25519_PUB": ("id_ed25519.pub", 0o644),
        "SSH_RSA_PRIV": ("id_rsa", 0o600),
        "SSH_RSA_PUB": ("id_rsa.pub", 0o644),
    }

    found = 0
    for s in secrets:
        key = s["secretKey"]
        if key in key_map:
            filename, mode = key_map[key]
            filepath = os.path.join(ssh_dir, filename)
            with open(filepath, 'w') as f:
                f.write(s["secretValue"])
            os.chmod(filepath, mode)
            ok(f"{filename} ({oct(mode)})")
            found += 1

    if found == 0:
        warn("No SSH keys found in secrets")
    return found > 0

def setup_github(secrets):
    """Configure gh CLI if GH_PAT or GH_PAT_CLASSIC found"""
    header("GitHub CLI")

    gh_pat = None
    for s in secrets:
        if s["secretKey"] in ("GH_PAT", "GH_PAT_CLASSIC"):
            gh_pat = s["secretValue"]
            break

    if not gh_pat:
        warn("No GitHub PAT found in secrets")
        return False

    if not run("which gh", check=False):
        warn("gh CLI not installed, skipping")
        return False

    result = run(f'echo "{gh_pat}" | gh auth login --with-token 2>&1', check=False)
    status = run("gh auth status 2>&1", check=False)
    if "Logged in" in (status or ""):
        user = run('gh api user --jq ".login" 2>/dev/null', check=False)
        ok(f"GitHub: {user or 'authenticated'}")
        return True
    else:
        fail("GitHub auth failed")
        return False

def main():
    parser = argparse.ArgumentParser(description="Pull Infisical secrets and configure Linux server")
    parser.add_argument("--env", default="dev", help="Environment (default: dev)")
    parser.add_argument("--project", default=None, help="Project ID (auto-detected if not set)")
    parser.add_argument("--dry-run", action="store_true", help="Show secrets without writing")
    parser.add_argument("--no-ssh", action="store_true", help="Skip SSH key setup")
    parser.add_argument("--no-github", action="store_true", help="Skip GitHub CLI setup")
    parser.add_argument("--no-bashrc", action="store_true", help="Skip bashrc write")
    args = parser.parse_args()

    print(f"\n{BOLD}{'='*60}{NC}")
    print(f"{BOLD}  Infisical Secret Setup{NC}")
    print(f"{BOLD}{'='*60}{NC}")

    if not check_prerequisites():
        sys.exit(1)

    token = login()
    if not token:
        sys.exit(1)

    project_id = args.project or INFISICAL_PROJECT_ID
    if not project_id:
        projects = get_projects(token)
        if not projects:
            sys.exit(1)
        # Use first project
        project_id = projects[0]["id"]
        info(f"Auto-selected project: {projects[0]['name']}")

    secrets = get_secrets(token, project_id, args.env)
    if not secrets:
        sys.exit(1)

    env_script = generate_env_script(secrets, args.env)

    if args.dry_run:
        header("Dry Run - Generated Script")
        print(env_script)
        return

    if not args.no_bashrc:
        write_to_bashrc(env_script, args.env)

    if not args.no_ssh:
        setup_ssh_keys(secrets)

    if not args.no_github:
        setup_github(secrets)

    # Summary
    header("Summary")
    secret_names = [s["secretKey"] for s in secrets]
    print(f"  Environment: {args.env}")
    print(f"  Secrets:     {len(secrets)}")
    print(f"  Keys:        {', '.join(secret_names)}")
    print()
    ok("All done! Run 'source ~/.bashrc' or open new terminal.")
    print()

if __name__ == "__main__":
    main()
