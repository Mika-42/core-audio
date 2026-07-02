from pathlib import Path
import subprocess
import shlex
import re

AI_name = "mkAI"
claude_code_AI = "hf.co/ruv/ruvltra-claude-code:Q4_K_M"

def ask_AI(prompt, model):
    result = subprocess.run(["ollama", "run", model, prompt], capture_output=True, text=True)
    return result.stdout.strip()

def ask_AI_with_files(prompt, model):
    prompt_with_code = f"{get_formated_files()}{prompt}"
    print(prompt_with_code)
    print("\n===AI Answer ===\n")
    return ask_AI(prompt_with_code, model)

def run_command(cmd):
    try:
        result = subprocess.run(
            shlex.split(cmd), capture_output=True, text=True, check=True
        )
        return result.stdout
    except subprocess.CalledProcessError as e:
        return f"Error : {e.stderr}"
#==============================================

def get_repository_file_list() -> list:
   return run_command("git ls-files").splitlines(); 


def get_formated_files():
    files = get_repository_file_list()
    repo_files = '';
    for file in files:
        path = Path(file)

        try:
            content = path.read_text()
        except Exception:
            continue

        lang = path.suffix[1:] if path.suffix else ""

        repo_files += f"# file: {file}\n```{lang}\n{content.rstrip()}\n```\n"
    return repo_files

#=============================================
def new_branch(branch_name):
    return run_command(f"git switch -c {branch_name}")

def get_current_branch_name():
    return run_command("git rev-parse --abbrev-ref HEAD")

def update_branch(branch_name, commit_msg):
    if get_current_branch_name() != branch_name:
        return "Error : Current branch name mismatch with desired branch name."

    run_command("git add .")
    run_command(f"git commit -m \"{commit_msg}\"")
#----------------------------
get_branch_name_AI_prompt = """
Respond only with a Git branch name in a few words that descripe the intention of the dev. 
- No introduction, explanation, justification, or comment.  
- No full sentences, only the branch name.  
- Convert everything to lowercase and replace spaces or underscores with hyphens "-".  
- Do not put anything after the branch name.  

Context: {prompt}
"""
def clean_branch_name(name: str) -> str:
    # Prend uniquement la première ligne
    name = name.strip().split("\n")[0]
    # Convertit en minuscules
    name = name.lower()
    # Remplace espaces et underscores par "-"
    name = re.sub(r"[ _]+", "-", name)
    # Supprime tous les caractères non autorisés (garder lettres, chiffres, / et -)
    name = re.sub(r"[^a-z0-9/-]", "", name)
    return name

def get_branch_name_AI(usr_prompt) -> str: 
    name = ask_AI(get_branch_name_AI_prompt.format(prompt=usr_prompt), claude_code_AI)
    return f"{AI_name}/{clean_branch_name(name)}"

if __name__ == "__main__":
    while True:
        prompt = input(">>> ")
        if prompt.startswith((":h", ":help")):
            print(":h or :help  => show help menu")
            print(":q or :quit  => close the agent")
            print(":f or :files => add repo file to request")
        
        elif prompt.startswith((":q", ":quit")):
            break
        
        elif prompt.startswith((":f", ":files")):
            print(ask_AI_with_files(prompt, claude_code_AI));
        else:
            print(ask_AI(prompt, claude_code_AI));
