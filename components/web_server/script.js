// script.js
// This script provides client-side functionality for interacting with the ESP32 web server.
// It handles file operations (upload, delete), displays messages, and dynamically updates content.

console.log("script.js started loading.");

/**
 * @function displayFileOperationMessage
 * @description Displays a temporary status message to the user for file operations.
 * Messages are shown in a dedicated container and styled based on their type.
 * @param {string} type - The type of message ('success', 'error', 'info'). Used as a CSS class.
 * @param {string} message - The text content of the message to display.
 */
function displayFileOperationMessage(type, message) {
    console.log(`displayFileOperationMessage called: Type=${type}, Message="${message}"`);
    let messageContainer = document.getElementById('file-operation-message-container');

    // Fallback: If container not found, try to create and append it dynamically.
    if (!messageContainer) {
        console.error('Message container #file-operation-message-container not found! Attempting to create it.');
        messageContainer = document.createElement('div');
        messageContainer.id = 'file-operation-message-container';
        const mainContent = document.querySelector('.container main');
        if (mainContent) {
            mainContent.prepend(messageContainer);
            console.log('#file-operation-message-container created and added to main.');
        } else {
            const containerDiv = document.querySelector('.container');
            if (containerDiv) {
                containerDiv.prepend(messageContainer);
                console.log('#file-operation-message-container created and added to .container.');
            } else {
                document.body.prepend(messageContainer);
                console.log('#file-operation-message-container created and added to body as last resort.');
            }
        }
        if (!messageContainer) {
            console.error('Error: #file-operation-message-container remained null after creation attempt!');
            return;
        }
    }

    // Set message content with appropriate CSS class.
    messageContainer.innerHTML = `<div class="message ${type}">${message}</div>`;
    console.log(`Message set in container: "${message}"`);

    // Clear any previous timeout to prevent messages from disappearing prematurely.
    if (messageContainer.timeoutId) {
        clearTimeout(messageContainer.timeoutId);
        console.log('Previous message timeout cleared.');
    }

    // Set a new timeout to hide the message after 7 seconds.
    messageContainer.timeoutId = setTimeout(() => {
        if (messageContainer) {
            messageContainer.innerHTML = '';
            console.log('Message hidden after timeout.');
        }
    }, 7000);
}

/**
 * @function refreshFileList
 * @description Asynchronously fetches an updated file list from the server and refreshes
 * the file table on the current page. Updates the table's tbody content dynamically
 * instead of a full page reload.
 */
function refreshFileList() {
    console.log('refreshFileList called.');
    // Optional: displayFileOperationMessage('info', 'Refreshing file list...');

    // Use Fetch API to send a GET request to the '/list' URI.
    // The server is expected to return the full HTML content of the file list page.
    fetch('/list')
        .then(response => {
            console.log('fetch(/list) response received. Status:', response.status);
            if (!response.ok) {
                throw new Error(`HTTP error! Status: ${response.status}`);
            }
            return response.text(); // Parse response as plain text (HTML).
        })
        .then(html => {
            console.log('fetch(/list) response parsed as text.');
            const parser = new DOMParser();
            const doc = parser.parseFromString(html, 'text/html');
            console.log('Parsed received HTML.');

            // Find the tbody element of the file table from the newly parsed document.
            const newTbody = doc.getElementById('file-table') ? doc.getElementById('file-table').querySelector('tbody') : null;
            console.log('Found newTbody:', newTbody);

            // Find the tbody element of the file table in the current browser DOM.
            const currentTbody = document.getElementById('file-table') ? document.getElementById('file-table').querySelector('tbody') : null;
            console.log('Found currentTbody:', currentTbody);

            if (currentTbody && newTbody) {
                // Replace the inner HTML of the current tbody with the new content.
                currentTbody.innerHTML = newTbody.innerHTML;
                console.log('Replaced current tbody with new tbody content.');

                // Re-attach delete listeners, ensuring event delegation is active for new elements.
                attachDeleteListeners();

                // Optional: Clear any "Refreshing list..." info message.
                const messageContainer = document.getElementById('file-operation-message-container');
                if (messageContainer && messageContainer.textContent.startsWith('Osvježavam listu')) {
                    messageContainer.innerHTML = '';
                    console.log('Cleared "Refreshing list..." message.');
                }
            } else {
                console.error('Error refreshing list: Table tbody not found.');
                displayFileOperationMessage('error', 'Error finding file list in server response.');
            }
        })
        .catch(error => {
            console.error('Error fetching file list:', error);
            if (!error.message.startsWith('HTTP error')) {
                displayFileOperationMessage('error', error.message || 'An unexpected error occurred while refreshing the file list.');
            }
        });
}

/**
 * @function attachDeleteListeners
 * @description Attaches a click event listener to the file table for handling individual file delete operations.
 * Uses event delegation: the listener is placed on the parent tbody element, and it checks
 * if the clicked element (or its closest parent) is a delete link. This is efficient
 * and works correctly when table content is dynamically updated.
 */
function attachDeleteListeners() {
    console.log('attachDeleteListeners called.');
    const fileTableBody = document.getElementById('file-table') ? document.getElementById('file-table').querySelector('tbody') : null;
    console.log('attachDeleteListeners: Found fileTableBody:', fileTableBody);

    if (fileTableBody) {
        // Prevent duplicate listeners by checking a custom property.
        if (fileTableBody.deleteListenerAttached) {
            console.log('Delete listener already attached to tbody. Skipping.');
            return;
        }

        // Attach 'click' listener to the tbody element.
        fileTableBody.addEventListener('click', async function(event) {
            console.log('Click event received on fileTableBody.');
            // Use event.target.closest() to find if a '.delete-link' was clicked within the tbody.
            const deleteLink = event.target.closest('.delete-link');
            console.log('Click event: Found deleteLink:', deleteLink);

            if (deleteLink) {
                event.preventDefault(); // Prevent default link navigation.
                event.stopPropagation(); // Stop event propagation.
                console.log('Delete link clicked. Default behavior prevented.');

                const deleteUrl = deleteLink.href;
                console.log('Delete URL:', deleteUrl);

                // Extract and decode filename from the URL for confirmation message.
                const url = new URL(deleteUrl);
                const filename = url.searchParams.get('file') || 'selected file';
                const decodedFilename = decodeURIComponent(filename);
                console.log('Filename for confirmation:', decodedFilename);

                // Show confirmation dialog before deleting.
                if (confirm(`Jeste li sigurni da želite obrisati datoteku '${decodedFilename}'?`)) {
                    console.log('Confirmation accepted. Proceeding with delete.');
                    displayFileOperationMessage('info', `Brišem datoteku '${decodedFilename}'...`);

                    try {
                        // Send GET request to the delete URL.
                        const response = await fetch(deleteUrl, {
                            method: 'GET',
                            headers: { 'Accept': 'application/json' }
                        });
                        console.log('Delete fetch response received. Status:', response.status);

                        if (!response.ok) {
                            let errorMsg = `Server error: ${response.status} ${response.statusText}`;
                            // Try to parse server's error message as JSON.
                            try {
                                const errorResult = await response.json();
                                console.log('Delete error response parsed as JSON:', errorResult);
                                errorMsg = errorResult.message || errorMsg;
                            } catch (e) {
                                console.log('Delete error response was not JSON.');
                            }
                            throw new Error(errorMsg);
                        }

                        // Parse successful response body as JSON.
                        const result = await response.json();
                        console.log('Delete fetch response parsed as JSON:', result);

                        // Display status message from the server response.
                        displayFileOperationMessage(result.status, result.message);

                        if (result.status === 'success') {
                            console.log('Delete successful. Removing table row.');
                            const rowToRemove = deleteLink.closest('tr');
                            if (rowToRemove) {
                                setTimeout(() => {
                                    rowToRemove.remove();
                                    console.log('Table row removed.');
                                    const tbody = document.getElementById('file-table').querySelector('tbody');
                                    if (tbody && tbody.children.length === 0) {
                                        console.log('Table is now empty.');
                                    }
                                }, 500);
                            }
                        }
                    } catch (error) {
                        console.error('Error during delete operation:', error);
                        displayFileOperationMessage('error', error.message || 'An unexpected network error occurred during delete.');
                    } finally {
                        console.log('Delete operation finished.');
                    }
                } else {
                    console.log('Confirmation denied. Delete cancelled.');
                    displayFileOperationMessage('info', 'File deletion cancelled.');
                }
            }
        });
        fileTableBody.deleteListenerAttached = true;
        console.log('Delete listener successfully attached to fileTableBody.');
    } else {
        console.error('Table tbody #file-table tbody not found for attaching delete listener.');
        displayFileOperationMessage('error', 'Internal error: File list did not load correctly (tbody not found).');
    }
}


// --- Main code execution after DOM is loaded ---
// document.addEventListener('DOMContentLoaded', ...) ensures that the code within this block
// executes only after the entire HTML document is loaded and the DOM tree is ready for manipulation.
// This is crucial because the code accesses elements (like the upload form, input fields, table)
// that must exist in the DOM before they can be interacted with.
document.addEventListener('DOMContentLoaded', () => {
    console.log('DOMContentLoaded event fired. Script execution started.');

    // Get references to the HTML elements for the upload form and file input field.
    const uploadForm = document.getElementById('uploadForm');
    console.log('Upload form found:', uploadForm);

    const fileInput = document.getElementById('filetoupload');
    console.log('File input found:', fileInput);

    // Get reference to the element for displaying the selected file name
    const selectedFileNameDisplay = document.getElementById('selectedFileName');
    // Get reference to the new "Delete All" button
    const deleteAllBtn = document.getElementById('deleteAllBtn');
    console.log('Delete All button found:', deleteAllBtn);


    // Add event listener for change on file input
    if (fileInput && selectedFileNameDisplay) {
        fileInput.addEventListener('change', () => {
            if (fileInput.files.length > 0) {
                selectedFileNameDisplay.textContent = `Odabrano: ${fileInput.files[0].name}`;
            } else {
                selectedFileNameDisplay.textContent = 'Nije odabrana datoteka';
            }
        });
    }


    if (uploadForm && fileInput) {
        console.log('Upload form and file input found. Attaching submit listener.');
        // Attach 'submit' event listener to the upload form.
        uploadForm.addEventListener('submit', function(e) {
            console.log('Upload form submit event fired.');
            e.preventDefault(); // IMPORTANT: Prevent default form behavior that would cause a full page refresh.

            // Adapt for custom file input: The 'filetoupload' input is hidden.
            // When user clicks the custom label, the hidden input's files property gets populated.
            if (fileInput.files.length === 0) {
                console.warn('No file selected for upload.');
                displayFileOperationMessage('error', 'Molimo odaberite datoteku za upload.');
                return;
            }
            const fileToUpload = fileInput.files[0];
            const formData = new FormData();
            formData.append('filetoupload', fileToUpload);

            displayFileOperationMessage('info', `Učitavanje "${fileToUpload.name}"...`);
            console.log('Displaying initial upload info message.');

            /**
             * @function sendUploadRequest
             * @description Helper function to send the upload request (initial or overwrite retry).
             * @param {string} url - The URL to send the request to.
             * @param {FormData} data - The FormData object containing the file.
             * @param {boolean} isRetry - True if this is a retry attempt after a conflict.
             * @returns {Promise<Object>} A Promise that resolves with the server's JSON response or a status object.
             */
            const sendUploadRequest = (url, data, isRetry = false) => {
                console.log(`Sending ${isRetry ? 'overwrite' : 'initial'} upload request to ${url}`);
                return fetch(url, {
                    method: uploadForm.method,
                    body: data,
                })
                .then(response => {
                    console.log(`Upload fetch response received (isRetry: ${isRetry}). Status:`, response.status);

                    if (!isRetry && response.status === 409) { // HTTP 409 Conflict for file existence on initial upload.
                        console.log('Conflict detected (HTTP 409) on initial upload.');
                        return response.json().then(conflictData => {
                            console.log('Conflict JSON data:', conflictData);
                            if (confirm(conflictData.message)) {
                                console.log('User confirmed overwrite. Preparing retry request.');
                                displayFileOperationMessage('info', `Overwriting file "${conflictData.filename || fileToUpload.name}"...`);
                                const overwriteUrl = `${uploadForm.action}?overwrite=true`;
                                return sendUploadRequest(overwriteUrl, data, true);
                            } else {
                                console.log('User cancelled overwrite.');
                                displayFileOperationMessage('info', 'Upload cancelled by user.');
                                return Promise.resolve({ status: 'cancelled', message: 'Upload cancelled.' });
                            }
                        });

                    } else if (!response.ok) {
                        // Handle other non-OK HTTP statuses (e.g., 400 Bad Request, 500 Internal Server Error).
                        console.error(`HTTP error during ${isRetry ? 'overwrite' : 'initial'} upload:`, response.status, response.statusText);
                        return response.json().then(err => {
                            console.error('Server responded with JSON error:', err);
                            throw { status: 'error', message: err.message || `HTTP error! Status: ${response.status}` };
                        }).catch(() => {
                            console.error('Server responded without JSON error.');
                            throw { status: 'error', message: `HTTP error! Status: ${response.status}` };
                        });
                    } else {
                        // Handle successful upload (HTTP 2xx status).
                        console.log(`${isRetry ? 'overwrite' : 'initial'} upload successful (HTTP 2xx). Parsing JSON response.`);
                        return response.json();
                    }
                });
            }; // End sendUploadRequest function.

            // --- Start the upload process ---
            sendUploadRequest(uploadForm.action, formData, false)
            .then(finalResult => {
                console.log('Final upload process result:', finalResult);

                if (finalResult.status === 'cancelled') {
                    console.log('Upload proces zavrsen zbog otkazivanja od strane korisnika.');
                } else {
                    if (finalResult && typeof finalResult.status === 'string' && typeof finalResult.message === 'string') {
                        displayFileOperationMessage(finalResult.status, finalResult.message);
                    } else {
                        console.error('Unexpected structure of final result:', finalResult);
                        displayFileOperationMessage('error', 'An unexpected error occurred processing server response.');
                    }

                    if (finalResult.status === 'success') {
                        console.log('Final upload status is success. Refreshing file list.');
                        refreshFileList();
                    }
                }
                uploadForm.reset(); // Reset the form, which also clears the file input
            })
            .catch(error => {
                console.error('An error occurred during the upload process:', error);
                if (error && typeof error === 'object' && typeof error.status === 'string' && typeof error.message === 'string') {
                    displayFileOperationMessage(error.status, error.message);
                } else {
                    displayFileOperationMessage('error', error.message || 'An unexpected error occurred during upload.');
                }
            });
        });
    } else {
        console.error('Upload form (#uploadForm) or file input (#filetoupload) not found on the page.');
        displayFileOperationMessage('error', 'Internal error: Web page did not load correctly (JS elements not found).');
    }

    // --- NOVO: Logic for "Delete All" button ---
    if (deleteAllBtn) {
        deleteAllBtn.addEventListener('click', async () => {
            if (confirm("Jeste li sigurni da želite OBRISATI SVE datoteke sa SD kartice? Ova radnja je nepovratna!")) {
                deleteAllBtn.disabled = true; // Disable button during operation
                deleteAllBtn.textContent = 'Brišem...'; // Change text to indicate processing
                displayFileOperationMessage('info', 'Brišem sve datoteke...');

                try {
                    // This is the endpoint that needs to be implemented in web_server.c
                    const response = await fetch('/delete_all', {
                        method: 'GET', // Using GET for simplicity, consider POST for destructive actions
                        headers: { 'Accept': 'application/json' }
                    });

                    if (!response.ok) {
                        let errorMsg = `Server error: ${response.status} ${response.statusText}`;
                        try {
                            const errorResult = await response.json();
                            errorMsg = errorResult.message || errorMsg;
                        } catch (e) { /* ignore */ }
                        throw new Error(errorMsg);
                    }

                    const result = await response.json(); // Expect JSON response
                    displayFileOperationMessage(result.status, result.message);

                    if (result.status === 'success') {
                        refreshFileList(); // Refresh list after successful deletion
                    }
                } catch (error) {
                    console.error('Greška prilikom brisanja svih datoteka:', error);
                    displayFileOperationMessage('error', error.message || 'Mrežna greška prilikom brisanja svih datoteka.');
                } finally {
                    deleteAllBtn.disabled = false; // Re-enable button
                    deleteAllBtn.textContent = 'Obriši sve'; // Restore button text
                }
            } else {
                displayFileOperationMessage('info', 'Brisanje svih datoteka otkazano.');
            }
        });
    }


    // --- Inicijalizacija listenera za brisanje kada se stranica učita ---
    // This ensures that delete links in the initial (embedded HTML) list are functional.
    // Due to event delegation, the listener will also work for dynamically added links later.
    console.log('Calling attachDeleteListeners.');
    attachDeleteListeners();
}); // End DOMContentLoaded listener.