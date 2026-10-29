#!/usr/bin/env python3
"""Regression cases for immutable package and shared-source selection."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('qctsm_selection', Path(__file__).with_name('qctsm-artifact-selection.py'))
selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selection)
HEAD = 'a' * 40


def inventory():
    jobs, artifacts = [], []
    names = ['qctsm '+identifier for identifier in selection.CONFIGURATIONS]+['qctsm shared core source']
    for index,name in enumerate(names,1):
        jobs.append({'id':index, 'run_id':42, 'head_sha':HEAD, 'run_attempt':1,
            'workflow_name':'qctsm native builds', 'head_branch':'main', 'name':name,
            'created_at':'2026-01-01T09:59:00Z', 'started_at':'2026-01-01T10:00:00Z',
            'completed_at':'2026-01-01T10:01:00Z', 'status':'completed', 'conclusion':'success',
            'runner_id':index, 'runner_name':'runner-'+str(index), 'labels':['test'],
            'steps':[{'number':1,'name':'Build and verify','status':'completed','conclusion':'success',
                      'started_at':'2026-01-01T10:00:00Z','completed_at':'2026-01-01T10:01:00Z'}]})
        package_names = ({'qctsm-core-source-'+HEAD} if name == 'qctsm shared core source' else
            selection.package_names(name.removeprefix('qctsm '), selection.project_version(selection.SOURCE)))
        for package in sorted(package_names):
            artifacts.append({'id':len(artifacts)+1, 'name':package, 'created_at':'2026-01-01T10:00:50Z',
                'workflow_run':{'id':42,'head_sha':HEAD}, 'digest':'sha256:'+'b'*64,
                'expired':False, 'size_in_bytes':1024})
    return jobs,artifacts


class ArtifactSelectionTests(unittest.TestCase):
    def select(self,jobs,artifacts,attempt=1):
        return selection.select('xiaobing318/QCopilots-ToolSemanticMatching','42',HEAD,attempt,jobs,artifacts)

    def test_complete_exact_execution_has_twenty_one_packages_and_core(self):
        report = self.select(*inventory())
        self.assertTrue(report['passed'],report['errors'])
        self.assertEqual(sum(len(row['artifacts']) for row in report['selected'].values()),21)
        self.assertEqual(report['coreSource']['artifactName'],'qctsm-core-source-'+HEAD)

    def test_missing_core_fails_even_when_all_native_variants_pass(self):
        jobs,artifacts = inventory()
        report = self.select(jobs,artifacts[:-1])
        self.assertFalse(report['passed'])
        self.assertTrue(any('shared core' in error for error in report['errors']))

    def test_mismatched_core_head_or_digest_fails(self):
        for field,value in [('digest','not-a-digest'),('workflow_run',{'id':42,'head_sha':'c'*40})]:
            jobs,artifacts = inventory()
            artifacts[-1][field] = value
            self.assertFalse(self.select(jobs,artifacts)['passed'])

    def test_copied_successes_resolve_to_original_execution(self):
        jobs,artifacts = inventory()
        copied = copy.deepcopy(jobs)
        for row in copied:
            row.update(id=row['id']+100,run_attempt=2,created_at='2026-01-01T10:10:00Z')
        report = self.select(jobs+copied,artifacts,2)
        self.assertTrue(report['passed'],report['errors'])
        self.assertEqual(report['coreSource']['sourceAttempt'],1)
        self.assertTrue(all(row['sourceAttempt']==1 for row in report['selected'].values()))

    def test_failed_latest_native_or_core_cannot_borrow_success(self):
        for failed in (0,-1):
            jobs,artifacts = inventory()
            copied = copy.deepcopy(jobs)
            for row in copied:
                row.update(id=row['id']+100,run_attempt=2,created_at='2026-01-01T10:10:00Z')
            copied[failed]['conclusion'] = 'failure'
            self.assertFalse(self.select(jobs+copied,artifacts,2)['passed'])

    def test_package_from_outside_original_execution_is_rejected(self):
        jobs,artifacts = inventory()
        artifacts[0]['created_at'] = '2026-01-01T10:02:00Z'
        self.assertFalse(self.select(jobs,artifacts)['passed'])

    def test_skipped_native_job_cannot_satisfy_the_required_matrix(self):
        jobs,artifacts = inventory()
        jobs[0]['conclusion'] = 'skipped'
        self.assertFalse(self.select(jobs,artifacts)['passed'])


if __name__ == '__main__':
    unittest.main()
