/**
 * LightNVR Web Interface — Face Library Page
 */

import { render } from 'preact';
import { FacesView } from '../components/preact/FacesView.jsx';
import { QueryClientProvider, queryClient } from '../query-client.js';
import { Header } from '../components/preact/Header.jsx';
import { Footer } from '../components/preact/Footer.jsx';
import { ToastContainer } from '../components/preact/ToastContainer.jsx';
import { setupSessionValidation } from '../utils/auth-utils.js';
import { initI18n } from '../i18n.js';

document.addEventListener('DOMContentLoaded', async () => {
    await initI18n();
    setupSessionValidation();

    const container = document.getElementById('main-content');
    if (container) {
        render(
            <QueryClientProvider client={queryClient}>
                <Header />
                <ToastContainer />
                <FacesView />
                <Footer />
            </QueryClientProvider>,
            container
        );
    }
});
