"""核对整理副本、原始归档及新编写文档的本地链接。"""
from pathlib import Path
from urllib.parse import unquote
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    failures = []
    current_originals = json.loads((ROOT / 'docs/original_files.json').read_text(encoding='utf-8'))
    history = ROOT / 'archive/previous_organization'
    full_manifest = history / 'docs/original_files.json'
    originals = json.loads(full_manifest.read_text(encoding='utf-8')) if full_manifest.exists() else []
    mapping = json.loads((ROOT / 'docs/file_mapping.json').read_text(encoding='utf-8'))
    excluded_path = history / 'excluded_file_mapping.json'
    excluded = json.loads(excluded_path.read_text(encoding='utf-8')) if excluded_path.exists() else []
    for row in originals:
        p = ROOT / row['archive_path']
        if not p.is_file() or p.stat().st_size != row['bytes'] or sha(p) != row['sha256']:
            failures.append('原件异常: ' + row['archive_path'])
    local_only_skipped = []
    for row in [*mapping, *excluded]:
        p = ROOT / row['target']
        if not p.exists() and row.get('local_only'):
            local_only_skipped.append(row['target'])
            continue
        if not p.is_file() or sha(p) != row['sha256']:
            failures.append('整理副本异常: ' + row['target'])
    authored = ['README.md', 'docs/SETUP.md', 'docs/VERSION_STATUS.md',
                'docs/IMAGE_SOURCES.md', 'src/ra8p1/README.md']
    checked_links = 0
    for relative in authored:
        p = ROOT / relative
        for link in re.findall(r'\]\(([^)]+)\)', p.read_text(encoding='utf-8')):
            if re.match(r'\w+://|#', link):
                continue
            target = (p.parent / unquote(link.split('#')[0])).resolve()
            checked_links += 1
            if not target.exists():
                failures.append('链接失效: ' + relative + ' -> ' + link)
    image_rows = [row for row in mapping if row['target'].startswith('images/')]
    for row in image_rows:
        if '.pptx!' not in row['source'] or 'source_slide' not in row:
            failures.append('展示图片缺少PPT来源: ' + row['target'])
    for directory in ['src', 'images', 'materials']:
        for child in (ROOT / directory).iterdir():
            if child.is_dir() and child.name != 'ra8p1':
                failures.append('展示区存在其他项目: ' + str(child.relative_to(ROOT)))
    result = {'original_files_verified': len(originals), 'source_original_file_count': len(current_originals),
              'organized_files_verified': len(mapping) - len(local_only_skipped),
              'local_only_files_skipped': local_only_skipped,
              'local_archives_available': full_manifest.exists(), 'archived_copies_verified': len(excluded),
              'ppt_images_verified': len(image_rows),
              'authored_local_links_checked': checked_links, 'failures': failures,
              'scope': '文件完整性与文档链接；不含编译、模型精度和硬件验收'}
    (ROOT / 'docs/verification_result.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
