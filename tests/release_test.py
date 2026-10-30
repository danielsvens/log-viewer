"""Exercise release versioning and notes against real Git histories."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('release_notes', Path(__file__).resolve().parents[1] / '.github/scripts/release_notes.py')
release_notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_notes)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.previous = os.getcwd()
        os.chdir(self.directory.name)
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.name', 'Release Test')
        self.git('config', 'user.email', 'release@example.invalid')
        self.commit('initial commit')

    def tearDown(self):
        os.chdir(self.previous)
        self.directory.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', *args], text=True).strip()

    def commit(self, subject, body=None):
        args = ['commit', '--allow-empty', '-q', '-m', subject]
        if body:
            args.extend(['-m', body])
        self.git(*args)
        return self.git('rev-parse', 'HEAD')

    def plan(self):
        return release_notes.plan('owner/log-viewer', 'HEAD')

    def test_initial_and_legacy_migration(self):
        tag, notes = self.plan()
        self.assertEqual(tag, 'v0.1.0')
        self.assertIn('- initial commit', notes)
        self.assertIn('/commits/v0.1.0', notes)
        self.git('tag', 'build-1')
        self.commit('fix: release names')
        tag, notes = self.plan()
        self.assertEqual(tag, 'v0.1.0')
        self.assertNotIn('- initial commit', notes)
        self.assertIn('/compare/build-1...v0.1.0', notes)

    def test_workflow_output_files(self):
        subprocess.run(['python3', release_notes.__file__, '--repository', 'owner/log-viewer',
                        '--notes', 'notes.md', '--output', 'outputs'], check=True, capture_output=True)
        self.assertEqual(Path('outputs').read_text(), 'tag=v0.1.0\n')
        self.assertIn('- initial commit', Path('notes.md').read_text())

    def test_patch_minor_and_major(self):
        self.git('tag', 'v0.1.0')
        self.commit('fix: parsing')
        self.assertEqual(self.plan()[0], 'v0.1.1')
        self.git('tag', 'v0.1.1')
        self.commit('feat(query): grouping')
        self.assertEqual(self.plan()[0], 'v0.2.0')
        self.git('tag', 'v0.2.0')
        self.commit('feat!: change query syntax')
        self.assertEqual(self.plan()[0], 'v1.0.0')
        self.git('tag', 'v1.0.0')
        self.commit('refactor: change interface', 'BREAKING CHANGE: remove old API')
        self.assertEqual(self.plan()[0], 'v2.0.0')

    def test_rerun_preserves_version_and_notes(self):
        self.git('tag', 'v0.1.0')
        self.commit('fix: rerun')
        first = self.plan()
        self.git('tag', first[0])
        self.assertEqual(self.plan(), first)

    def test_pull_requests_and_direct_commits(self):
        self.git('tag', 'v0.1.0')
        self.commit('feat: source tree (#12)')
        direct = self.commit('fix: empty input')
        self.git('checkout', '-q', '-b', 'feature')
        self.commit('feat: worker support')
        self.git('checkout', '-q', 'main')
        self.git('merge', '--no-ff', '-q', 'feature', '-m', 'Merge pull request #13 from owner/feature', '-m', 'Improve background search')
        tag, notes = self.plan()
        self.assertEqual(tag, 'v0.2.0')
        self.assertIn('- feat: source tree ([#12](https://github.com/owner/log-viewer/pull/12))', notes)
        self.assertIn('- Improve background search ([#13]', notes)
        self.assertIn(f'/commit/{direct}', notes)
        self.assertNotIn('- feat: worker support', notes)
        self.assertIn('/compare/v0.1.0...v0.2.0', notes)

    def test_breaking_commit_inside_merge_and_unrelated_tag(self):
        self.git('tag', 'v0.1.0')
        self.git('checkout', '-q', '-b', 'feature')
        self.commit('fix!: incompatible input')
        self.git('checkout', '-q', 'main')
        self.git('merge', '--no-ff', '-q', 'feature', '-m', 'Merge pull request #9 from owner/feature')
        self.assertEqual(self.plan()[0], 'v1.0.0')
        self.git('checkout', '-q', '-b', 'unrelated', 'HEAD~1')
        self.commit('unreleased change')
        self.git('tag', 'v99.0.0')
        self.git('checkout', '-q', 'main')
        self.assertEqual(self.plan()[0], 'v1.0.0')


if __name__ == '__main__':
    unittest.main()
