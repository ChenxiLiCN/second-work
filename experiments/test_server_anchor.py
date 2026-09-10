import test_server_selected

class AnchorRunnerTests(test_server_selected.SelectedRunnerTests):
    anchor=True

    def test_counter_accounting(self):
        self.exercise('accounting')
